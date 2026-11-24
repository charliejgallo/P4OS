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

#include <pthread.h>
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
            case AOS_154_OP_HELLO:
            case AOS_154_OP_UNGUARD:
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
