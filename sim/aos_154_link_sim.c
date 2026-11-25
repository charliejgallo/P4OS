/*
 * P4OS simulator - a make-believe C6 for the 802.15.4 radio (aos_154.h):
 * it answers the P4's requests as c6/main/radio154.c does, with a radio
 * that hears an invented neighbourhood: a Zigbee network on channel 15 (a
 * coordinator and two sensors talking, beacon requests now and then), a
 * Thread network on 25, and a Wi-Fi access point on Wi-Fi channel 6 that
 * raises the energy of 802.15.4 channels 16 to 19. Nobody's real traffic.
 *
 * P4_SIM_154=0 makes it a C6 with an older firmware: it answers nothing.
 */
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint8_t b[8]; size_t n; } cmd_t;

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static cmd_t s_q[8];
static int s_qn;
static uint8_t s_state, s_channel;
static uint32_t s_frames;
static uint8_t s_seq = 1;
static uint8_t s_zb;

static uint16_t crc_kermit(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

static uint64_t now_us(void) { return aos_hal_uptime_ms() * 1000ull; }

static void status(int8_t err)
{
    aos_154_status_t st = {
        .proto = AOS_154_PROTO, .state = s_state, .channel = s_state ? s_channel : 0, .frames = s_frames,
        .uptime_s = (uint32_t)(aos_hal_uptime_ms() / 1000), .err = err,
        .heap_free = 150000, .heap_min = 120000,
        .zb = s_zb, .zb_channel = s_state == AOS_154_ZIGBEE ? s_channel : 0,
        .zb_heap_before = s_zb ? 78000 : 0, .zb_heap_after = s_zb == AOS_154_ZB_FORMED ? 41000 : 0,
    };
    aos_154_on_msg(AOS_154_MSG_STATUS, (const uint8_t *)&st, sizeof st);
}

/* one frame of the neighbourhood heard on 'ch', into buf (PSDU with FCS) */
static int make_frame(int ch, uint8_t *f)
{
    int n = 0, kind = rand() % 10;
    if (ch == 15) {
        if (kind == 0) {                /* a beacon request, somebody looking for networks */
            f[n++] = 0x03; f[n++] = 0x08; f[n++] = s_seq++;
            f[n++] = 0xff; f[n++] = 0xff; f[n++] = 0xff; f[n++] = 0xff; f[n++] = 0x07;
        } else if (kind < 4) {          /* an acknowledgement */
            f[n++] = 0x02; f[n++] = 0x00; f[n++] = (uint8_t)(s_seq - 1);
        } else {                        /* Zigbee data, a sensor to the coordinator or a link status */
            bool bc = kind == 9;
            uint16_t src = kind & 1 ? 0x7c3b : 0x51a2, dst = bc ? 0xffff : 0x0000;
            f[n++] = bc ? 0x41 : 0x61; f[n++] = 0x88; f[n++] = s_seq++;
            f[n++] = 0x62; f[n++] = 0x1a;                       /* PAN 0x1a62 */
            f[n++] = (uint8_t)dst; f[n++] = (uint8_t)(dst >> 8);
            f[n++] = (uint8_t)src; f[n++] = (uint8_t)(src >> 8);
            f[n++] = bc ? 0x09 : 0x48; f[n++] = 0x02;           /* NWK: data or command, security */
            f[n++] = (uint8_t)dst; f[n++] = (uint8_t)(dst >> 8);
            f[n++] = (uint8_t)src; f[n++] = (uint8_t)(src >> 8);
            f[n++] = bc ? 1 : 30; f[n++] = (uint8_t)rand();
            f[n++] = 0x28;                                      /* the security header, then a ciphertext */
            int len = 20 + rand() % 30;
            for (int i = 0; i < len; i++) f[n++] = (uint8_t)rand();
        }
    } else if (ch == 25) {              /* Thread: 6LoWPAN between two long addresses */
        f[n++] = 0x41; f[n++] = 0xcc; f[n++] = s_seq++;
        f[n++] = 0xce; f[n++] = 0xfa;                           /* PAN 0xface */
        for (int i = 0; i < 16; i++) f[n++] = (uint8_t)(0x10 + i);
        f[n++] = 0x7a; f[n++] = 0x33; f[n++] = 0x3a;            /* IPHC, ICMPv6 */
        int len = 10 + rand() % 40;
        for (int i = 0; i < len; i++) f[n++] = (uint8_t)rand();
    } else return 0;
    uint16_t c = crc_kermit(f, (size_t)n);
    f[n++] = (uint8_t)c;
    f[n++] = (uint8_t)(c >> 8);
    return n;
}

static int8_t energy(int ch)
{
    int base = -96 + rand() % 4;
    if (ch >= 16 && ch <= 19) base = -72 + rand() % 14;         /* the Wi-Fi access point on its channel 6 */
    if (ch == 15 || ch == 25) base = -88 + rand() % 20;
    return (int8_t)base;
}

static void *thread(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c = { .n = 0 };
        pthread_mutex_lock(&s_mx);
        if (s_qn) {
            c = s_q[0];
            memmove(s_q, s_q + 1, sizeof s_q[0] * (size_t)--s_qn);
        }
        pthread_mutex_unlock(&s_mx);
        if (c.n) {
            int8_t err = 0;
            switch (c.b[0]) {
            case AOS_154_OP_SCAN: {
                int dwell = c.b[3] | c.b[4] << 8;
                s_state = AOS_154_SCANNING;
                for (int ch = c.b[1]; ch <= c.b[2]; ch++) {
                    s_channel = (uint8_t)ch;
                    usleep((useconds_t)dwell * 1000);
                    int sum = 0, max = -128;
                    for (int i = 0; i < 8; i++) { int e = energy(ch); sum += e; if (e > max) max = e; }
                    int fr = ch == 15 ? dwell / 40 + rand() % 4 : ch == 25 ? dwell / 120 + rand() % 2 : 0;
                    aos_154_scan_t r = { .channel = (uint8_t)ch, .ed_max = (int8_t)max, .ed_avg = (int8_t)(sum / 8),
                                         .samples = 8, .frames = (uint16_t)fr, .bytes = (uint16_t)(fr * 40),
                                         .dwell_ms = (uint16_t)dwell };
                    aos_154_on_msg(AOS_154_MSG_SCAN, (const uint8_t *)&r, sizeof r);
                }
                s_state = AOS_154_IDLE;
                break;
            }
            case AOS_154_OP_SNIFF:
                s_channel = c.b[1];
                s_frames = 0;
                s_state = AOS_154_SNIFFING;
                break;
            case AOS_154_OP_STOP:
                s_state = AOS_154_IDLE;
                break;
            case AOS_154_OP_ZB_START:
                s_state = AOS_154_ZIGBEE;
                s_channel = c.b[1];
                s_zb = AOS_154_ZB_FORMED;
                break;
            case AOS_154_OP_ZB_STOP:
                s_state = AOS_154_IDLE;
                s_zb = AOS_154_ZB_OFF;
                break;
            case AOS_154_OP_HELLO:
            case AOS_154_OP_UNGUARD:
            case AOS_154_OP_SLOTS:
            case AOS_154_OP_SLOTS_GROW: {   /* a factory C6 on ota_0, grown when asked */
                static uint8_t grown;
                int8_t g = 0;
                if (c.b[0] == AOS_154_OP_SLOTS_GROW) { g = grown ? 0 : 1; grown = 1; }
                aos_154_slots_t r = { .layout = grown ? AOS_154_SLOTS_GROWN : AOS_154_SLOTS_FACTORY, .grow = g,
                                      .slot_size = 0x180000, .running_addr = 0x10000 };
                aos_154_on_msg(AOS_154_MSG_SLOTS, (const uint8_t *)&r, sizeof r);
                break;
            }
            case AOS_154_OP_RCP_START:
            case AOS_154_OP_LOG:            /* the make-believe C6 has nothing to say */
                break;
            default:
                err = -1;
            }
            status(err);
        }
        if (s_state == AOS_154_SNIFFING) {
            /* 40 ms worth: a busy Zigbee network sends some 25 frames a second */
            uint8_t batch[1024];
            size_t at = 0;
            int k = s_channel == 15 ? rand() % 3 : s_channel == 25 ? (rand() % 4 == 0) : 0;
            for (int i = 0; i < k; i++) {
                uint8_t f[127];
                int n = make_frame(s_channel, f);
                if (!n) break;
                uint64_t t = now_us() + (uint64_t)i * 3000;
                aos_154_rec_t rec = { .len = (uint8_t)n, .rssi = (int8_t)(-50 - rand() % 40), .lqi = (uint8_t)(120 + rand() % 130),
                                      .channel = s_channel, .t_lo = (uint32_t)t, .t_hi = (uint32_t)(t >> 32) };
                memcpy(batch + at, &rec, sizeof rec);
                memcpy(batch + at + sizeof rec, f, (size_t)n);
                at += sizeof rec + (size_t)n;
                s_frames++;
            }
            if (at) aos_154_on_msg(AOS_154_MSG_FRAMES, batch, at);
            static int ticks;
            if (++ticks >= 25) {        /* the counters once a second, as radio154.c */
                ticks = 0;
                status(0);
            }
        }
        usleep(40000);
    }
    return NULL;
}

static bool off(void)
{
    const char *e = getenv("P4_SIM_154");
    return e && !strcmp(e, "0");
}

bool aos_154_link_open(void)
{
    if (off()) return true;
    pthread_t t;
    if (pthread_create(&t, NULL, thread, NULL)) return false;
    pthread_detach(t);
    return true;
}

bool aos_154_link_send(const uint8_t *data, size_t len)
{
    if (off()) return true;             /* taken, and never answered: an older C6 */
    pthread_mutex_lock(&s_mx);
    bool ok = s_qn < 8 && len && len <= 8;
    if (ok) {
        s_q[s_qn].n = len;
        memcpy(s_q[s_qn].b, data, len);
        s_qn++;
    }
    pthread_mutex_unlock(&s_mx);
    return ok;
}

/* the make-believe C6 has no RCP: spinel goes nowhere */
bool aos_154_link_spinel(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return !off();
}

/* no Zigbee stack in the simulator (aos_zigbee_p4.c is the board's) */
#include "aos_zigbee.h"
bool aos_zigbee_start(int channel) { (void)channel; return false; }
bool aos_zigbee_permit(int seconds) { (void)seconds; return false; }
bool aos_zigbee_permit_board(int seconds) { (void)seconds; return false; }
bool aos_zigbee_permit_strict(int seconds, bool board) { (void)seconds; (void)board; return false; }
void aos_zigbee_static_kn(bool on) { (void)on; }
bool aos_zigbee_remove(const uint8_t ieee[8]) { (void)ieee; return false; }
bool aos_zigbee_onoff(const uint8_t ieee[8], uint8_t ep, int cmd) { (void)ieee; (void)ep; (void)cmd; return false; }
bool aos_zigbee_level(const uint8_t ieee[8], uint8_t ep, int level) { (void)ieee; (void)ep; (void)level; return false; }
bool aos_zigbee_command(const uint8_t ieee[8], uint8_t ep, uint16_t cluster, uint8_t cmd, const void *data, int len,
                        uint16_t read_attr) { (void)ieee; (void)ep; (void)cluster; (void)cmd; (void)data; (void)len; (void)read_attr; return false; }
bool aos_zigbee_write(const uint8_t ieee[8], uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, const void *value,
                      int len) { (void)ieee; (void)ep; (void)cluster; (void)attr; (void)type; (void)value; (void)len; return false; }
bool aos_zigbee_tuya_dp(const uint8_t ieee[8], uint8_t dp, uint8_t type, int32_t value) { (void)ieee; (void)dp; (void)type; (void)value; return false; }
int aos_zigbee_permit_left(void) { return 0; }

/* ---- P4_SIM_ZB_IR=1: a network formed on channel 26 with a Tuya IR blaster
 * and a Tuya TS0601 temperature sensor (datapoints) in it, for the Zigbee app's card and the Infrarrojo app's sending through
 * it. Learning gives an NEC code (address 0x04, the command counting up) 3 s
 * after it starts; a sending is done in 0.8 s. Nothing goes anywhere. */

static const uint8_t SIM_IR_IEEE[8] = { 0x7a, 0x4b, 0x0c, 0xff, 0xfe, 0x8e, 0x2f, 0xa4 };
static struct { int state; int64_t t0; uint8_t cmd; char learned[400]; } s_sir;
static char s_sim_name[33];

static bool sim_ir(void)
{
    const char *e = getenv("P4_SIM_ZB_IR");
    return e && e[0] == '1';
}

static bool sim_emu_active(void);

void aos_zigbee_info(aos_zigbee_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (sim_emu_active()) {
        out->emu = true;
        return;
    }
    if (sim_ir()) {
        out->state = AOS_ZB_FORMED;
        out->channel = 26;
    }
}

int aos_zigbee_devices(aos_zigbee_dev_t *out, int max)
{
    if (!sim_ir() || max < 1) return 0;
    memset(out, 0, sizeof *out);
    memcpy(out->ieee, SIM_IR_IEEE, 8);
    out->nwk = 0x3c1d;
    out->ep = 1;
    out->step = AOS_ZB_DEV_DONE;
    out->clusters = AOS_ZB_CL_IR;
    out->cap = 0x8e;                    /* a router, mains powered */
    out->power = 0x01;
    out->rssi = -48;
    out->temp = INT16_MIN;
    out->hum = 0xFFFF;
    out->battery = 0xFF;
    out->voltage = 0;
    out->occupancy = 0xFF;
    out->zone_type = 0xFFFF;
    out->level = 0xFF;
    out->seen_ms = out->heard_ms = aos_hal_uptime_ms();
    snprintf(out->manuf, sizeof out->manuf, "_TZ3290_nba3knpsarkawgnt");
    snprintf(out->model, sizeof out->model, "TS1201");
    snprintf(out->name, sizeof out->name, "%s", s_sim_name);
    if (max < 2) return 1;
    /* and a Tuya temperature sensor that speaks in datapoints (0xEF00) */
    aos_zigbee_dev_t *t = &out[1];
    memset(t, 0, sizeof *t);
    static const uint8_t TIE[8] = { 0x31, 0x9c, 0x5a, 0xfe, 0xff, 0x22, 0x0b, 0xa4 };
    memcpy(t->ieee, TIE, 8);
    t->nwk = 0x7e02;
    t->ep = 1;
    t->step = AOS_ZB_DEV_DONE;
    t->clusters = AOS_ZB_CL_TUYA;
    t->cap = 0x80;
    t->power = 0x03;
    t->rssi = -61;
    t->temp = INT16_MIN;
    t->hum = 0xFFFF;
    t->battery = 0xFF;
    t->occupancy = 0xFF;
    t->zone_type = 0xFFFF;
    t->level = 0xFF;
    t->seen_ms = t->heard_ms = aos_hal_uptime_ms();
    snprintf(t->manuf, sizeof t->manuf, "_TZE200_ysm4dsb1");
    snprintf(t->model, sizeof t->model, "TS0601");
    static const struct { uint8_t dp, type; int32_t v; } DPS[] = { { 1, 2, 215 }, { 2, 2, 55 }, { 4, 2, 87 }, { 9, 4, 0 },
                                                                  { 104, 1, 1 } };
    for (size_t k = 0; k < sizeof DPS / sizeof DPS[0]; k++) {
        t->dps[k].dp = DPS[k].dp;
        t->dps[k].type = DPS[k].type;
        t->dps[k].v = DPS[k].v;
        t->dps[k].len = DPS[k].type == 2 ? 4 : 1;
        t->dps[k].ms = aos_hal_uptime_ms();
    }
    t->ndp = (uint8_t)(sizeof DPS / sizeof DPS[0]);
    return 2;
}

bool aos_zigbee_set_name(const uint8_t ieee[8], const char *name)
{
    if (!sim_ir() || memcmp(ieee, SIM_IR_IEEE, 8)) return false;
    snprintf(s_sim_name, sizeof s_sim_name, "%s", name ? name : "");
    return true;
}

int aos_zigbee_ir_blasters(uint8_t (*ieee)[8], char (*name)[33], int max)
{
    if (!sim_ir() || max < 1) return 0;
    memcpy(ieee[0], SIM_IR_IEEE, 8);
    snprintf(name[0], 33, "TS1201");
    return 1;
}

static void sim_ir_advance(void)
{
    int64_t now = (int64_t)aos_hal_uptime_ms();
    if (s_sir.state == AOS_ZB_IR_LEARNING && now - s_sir.t0 > 3000) {
        uint32_t d[68];
        int n = 0;
        uint32_t bits = 0x04u | (uint32_t)(uint8_t)~0x04 << 8 | (uint32_t)s_sir.cmd << 16 | (uint32_t)(uint8_t)~s_sir.cmd << 24;
        d[n++] = 9000;
        d[n++] = 4500;
        for (int k = 0; k < 32; k++) {
            d[n++] = 560;
            d[n++] = bits >> k & 1 ? 1690 : 560;
        }
        d[n++] = 560;
        s_sir.cmd++;
        if (aos_zigbee_ir_encode(d, n, s_sir.learned, sizeof s_sir.learned) > 0) s_sir.state = AOS_ZB_IR_LEARNED;
        else s_sir.state = AOS_ZB_IR_FAILED;
    } else if (s_sir.state == AOS_ZB_IR_SENDING && now - s_sir.t0 > 800) {
        s_sir.state = AOS_ZB_IR_SENT;
    }
}

bool aos_zigbee_ir_learn(const uint8_t ieee[8], bool start)
{
    if (!sim_ir() || memcmp(ieee, SIM_IR_IEEE, 8)) return false;
    s_sir.state = start ? AOS_ZB_IR_LEARNING : AOS_ZB_IR_IDLE;
    s_sir.t0 = (int64_t)aos_hal_uptime_ms();
    return true;
}

static bool sim_ir_send(const uint8_t ieee[8], int n, uint32_t freq)
{
    if (!sim_ir() || memcmp(ieee, SIM_IR_IEEE, 8) || n <= 0) return false;
    if (s_sir.state == AOS_ZB_IR_SENDING) return false;
    aos_hal_log("zigbee", "sim: the IR blaster sends %d durations at %u Hz", n, (unsigned)freq);
    s_sir.state = AOS_ZB_IR_SENDING;
    s_sir.t0 = (int64_t)aos_hal_uptime_ms();
    return true;
}

bool aos_zigbee_ir_send(const uint8_t ieee[8], const char *code_b64)
{
    static uint32_t d[2048];
    return sim_ir_send(ieee, aos_zigbee_ir_decode(code_b64, d, 2048), 38000);
}

bool aos_zigbee_ir_send_raw(const uint8_t ieee[8], const uint32_t *d, int n, uint32_t freq)
{
    (void)d;
    return sim_ir_send(ieee, n, freq ? freq : 38000);
}

int aos_zigbee_ir_state(uint8_t ieee[8], char *code, size_t n)
{
    sim_ir_advance();
    memcpy(ieee, sim_ir() ? SIM_IR_IEEE : (const uint8_t[8]){ 0 }, 8);
    if (n) snprintf(code, n, "%s", s_sir.learned);
    return s_sir.state;
}

/* ---- P4_SIM_ZB_EMU=1: a make-believe Home Assistant coordinator for the
 * board emulating a device (aos_zigbee_emu.c): it "opens" a network on
 * channel 15 three seconds after the board starts looking, lets it in,
 * interviews it as Zigbee2MQTT does (to fill the log) and switches the
 * light every 20 s. A switch that needs a restart on the board is made at
 * once here. Nothing goes anywhere. */

static struct {
    bool active, saved, tclk;
    int state;
    int64_t t0, joined, next_toggle;
    int step;
    int32_t v[AOS_ZBE_V_N];
    aos_zigbee_emu_log_t log[160];
    uint32_t n;
} s_emu = { .v = { 2150, 5000, 100, 0 } };

static bool sim_emu(void)
{
    const char *e = getenv("P4_SIM_ZB_EMU");
    return e && e[0] == '1';
}

static bool sim_emu_active(void) { return s_emu.active; }

static void emu_log(bool out, const char *fmt, ...)
{
    aos_zigbee_emu_log_t *e = &s_emu.log[s_emu.n % 160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);
    e->ms = (uint32_t)aos_hal_uptime_ms();
    e->out = out;
    s_emu.n++;
}

static void sim_emu_advance(void)
{
    if (!s_emu.active) return;
    int64_t now = (int64_t)aos_hal_uptime_ms();
    if (s_emu.state == AOS_ZBE_SEARCHING && now - s_emu.t0 > 3000) {
        s_emu.state = AOS_ZBE_JOINED;
        s_emu.joined = now;
        s_emu.saved = true;
        s_emu.step = 0;
        s_emu.next_toggle = now + 20000;
        emu_log(true, _("adentro: canal %u, PAN 0x%04x, la placa es 0x%04x"), 15u, 0x1A62u, 0x5C3Eu);
    }
    if (s_emu.state != AOS_ZBE_JOINED) return;
    /* Zigbee2MQTT's interview, in the firmware's own words (aos_zigbee_emu.c),
     * so a screenshot in any language reads as the board's would */
    while (s_emu.step < 15 && now - s_emu.joined > 400 * (s_emu.step + 1)) {
        switch (s_emu.step++) {
        case 0: emu_log(false, "%s", _("pidió el descriptor del nodo")); break;
        case 1: emu_log(false, "%s", _("pidió la lista de endpoints")); break;
        case 2:
            s_emu.tclk = true;
            emu_log(true, "%s", _("el coordinador le dio su propia clave de enlace (Zigbee 3.0)"));
            emu_log(false, _("pidió el descriptor del endpoint %u"), 1u);
            break;
        case 3: emu_log(false, _("pidió el descriptor del endpoint %u"), 2u); break;
        case 4: emu_log(false, _("leyó %s (ep %u):%s"), "Basic", 1u, " 0004 0000 0001 0005 0007 fffe"); break;
        case 5: emu_log(false, _("leyó %s (ep %u):%s"), "Basic", 1u, " 4000"); break;
        case 6: emu_log(false, _("ató %s (ep %u) a sí mismo"), "On/Off", 1u); break;
        case 7: emu_log(false, _("ató %s (ep %u) a sí mismo"), _("temperatura"), 2u); break;
        case 8: emu_log(false, _("ató %s (ep %u) a sí mismo"), _("humedad"), 2u); break;
        case 9: emu_log(false, _("ató %s (ep %u) a sí mismo"), _("batería"), 2u); break;
        case 10: emu_log(false, _("configuró reportes de %s 0x%04x: cada %u s a %u s"), "On/Off", 0u, 0u, 3600u); break;
        case 11: emu_log(false, _("configuró reportes de %s 0x%04x: cada %u s a %u s"), _("temperatura"), 0u, 10u, 3600u); break;
        case 12: emu_log(false, _("configuró reportes de %s 0x%04x: cada %u s a %u s"), _("humedad"), 0u, 10u, 3600u); break;
        case 13: emu_log(false, _("configuró reportes de %s 0x%04x: cada %u s a %u s"), _("batería"), 0x21u, 3600u, 62000u); break;
        case 14: emu_log(false, _("leyó %s (ep %u):%s"), "On/Off", 1u, " 0000"); break;
        }
    }
    if (now > s_emu.next_toggle) {
        s_emu.next_toggle = now + 20000;
        s_emu.v[AOS_ZBE_V_ON] = !s_emu.v[AOS_ZBE_V_ON];
        emu_log(false, _("mandó %s a la luz"), s_emu.v[AOS_ZBE_V_ON] ? _("Prender") : _("Apagar"));
        emu_log(false, _("la luz quedó %s"), s_emu.v[AOS_ZBE_V_ON] ? _("prendida") : _("apagada"));
    }
}

bool aos_zigbee_emu_join(int kind, bool restart_ok, bool *restart)
{
    if (restart) *restart = false;
    if (!sim_emu() || kind != AOS_ZBE_LIGHT_TEMP) return false;
    if (!s_emu.active && sim_ir()) {
        if (restart) *restart = true;
        if (!restart_ok) return false;
    }
    if (s_emu.active && s_emu.state == AOS_ZBE_JOINED) return true;
    s_emu.active = true;
    s_emu.state = AOS_ZBE_SEARCHING;
    s_emu.t0 = (int64_t)aos_hal_uptime_ms();
    emu_log(true, "%s", _("arrancando la pila como router"));
    emu_log(true, "%s", _("buscando una red abierta para sumarse"));
    return true;
}

bool aos_zigbee_emu_set(int value, int32_t v)
{
    if (value < 0 || value >= AOS_ZBE_V_N) return false;
    s_emu.v[value] = v;
    if (s_emu.active && value == AOS_ZBE_V_ON) emu_log(true, _("la luz, tocada en la placa: %s"), v ? _("prendida") : _("apagada"));
    return true;
}

bool aos_zigbee_emu_report_now(void)
{
    if (!s_emu.active || s_emu.state != AOS_ZBE_JOINED) return false;
    emu_log(true, "%s", _("mandó todos los valores al coordinador"));
    return true;
}

bool aos_zigbee_emu_leave(void)
{
    if (!s_emu.active) return false;
    emu_log(true, "%s", _("pidió irse de la red y olvidarla"));
    emu_log(true, "%s", _("salió de la red"));
    s_emu.saved = false;
    s_emu.active = false;               /* the board would restart into its own network */
    return true;
}

bool aos_zigbee_emu_home(void)
{
    s_emu.active = false;
    return true;
}

void aos_zigbee_emu_info(aos_zigbee_emu_t *out)
{
    sim_emu_advance();
    memset(out, 0, sizeof *out);
    out->state = s_emu.active ? (uint8_t)s_emu.state : AOS_ZBE_OFF;
    out->saved = s_emu.saved;
    out->tries = (uint16_t)(s_emu.active ? 1 + (aos_hal_uptime_ms() - s_emu.t0) / 5000 : 0);
    out->tclk = s_emu.tclk;
    memcpy(out->v, s_emu.v, sizeof s_emu.v);
    if (out->state == AOS_ZBE_JOINED) {
        out->channel = 15;
        out->pan = 0x1A62;
        out->nwk = 0x5C3E;
        out->joined_ms = (uint32_t)s_emu.joined;
    }
}

int aos_zigbee_emu_log(aos_zigbee_emu_log_t *out, uint32_t from, int max, uint32_t *next)
{
    sim_emu_advance();
    uint32_t first = s_emu.n > 160 ? s_emu.n - 160 : 0;
    if (from < first) from = first;
    int n = 0;
    for (uint32_t i = from; i < s_emu.n && n < max; i++) out[n++] = s_emu.log[i % 160];
    if (next) *next = from + (uint32_t)n;
    return n;
}
