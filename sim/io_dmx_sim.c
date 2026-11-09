/*
 * P4OS simulator - DMX512 backend (the front is components/aos_io/aos_io_dmx.c).
 *
 * One make-believe bus for every port. While an app sends, what it sends is
 * what the receivers get (the packets counted by the time gone at its
 * period). While nobody sends, a console plays a show on it at 40 packets a
 * second, 512 slots, BREAK 176 us and MAB 12 us:
 *
 *   1-24    eight RGB pixels, a rainbow running along them
 *   25-31   a 7-channel PAR: dimmer breathing, red, green, blue cycling,
 *           strobe 0, macro 0, speed 0
 *   32      a fog machine, on for 3 s every 20 s
 *
 * and, every 15 s, one packet with a BREAK of 60 us, which a receiver
 * counts as short. P4_SIM_DMX=0 takes the console off the bus (a receiver
 * then sees no signal).
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t t0_ms;
} sim_dmx_t;

static aos_io_dmx_t *s_sender;          /* the app sending, if one does */

static bool console_on(void)
{
    const char *e = getenv("P4_SIM_DMX");
    return !e || strcmp(e, "0");
}

static void hsv(float h, uint8_t *rgb)
{
    float r = fabsf(fmodf(h * 6.0f, 6.0f) - 3.0f) - 1.0f;
    float g = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 2.0f);
    float b = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 4.0f);
    rgb[0] = (uint8_t)(fminf(fmaxf(r, 0), 1) * 255);
    rgb[1] = (uint8_t)(fminf(fmaxf(g, 0), 1) * 255);
    rgb[2] = (uint8_t)(fminf(fmaxf(b, 0), 1) * 255);
}

static void console_packet(uint8_t *u, float t)
{
    memset(u, 0, 513);
    for (int p = 0; p < 8; p++) hsv(fmodf(t / 6.0f + p / 8.0f, 1.0f), u + 1 + 3 * p);
    u[25] = (uint8_t)(128 + 127 * sinf(t * 1.5f));
    hsv(fmodf(t / 10.0f, 1.0f), u + 26);
    u[32] = fmodf(t, 20.0f) < 3.0f ? 255 : 0;
}

bool aos_io_be_dmx_open(aos_io_dmx_t *d)
{
    sim_dmx_t *s = calloc(1, sizeof *s);
    if (!s) return false;
    s->t0_ms = aos_hal_uptime_ms();
    d->be = s;
    if (d->dir == AOS_DMX_SEND) s_sender = d;
    return true;
}

int aos_io_be_dmx_read(aos_io_dmx_t *d, uint8_t *out, int max, aos_dmx_stats_t *st)
{
    sim_dmx_t *s = d->be;
    uint64_t now = aos_hal_uptime_ms();
    if (d->dir == AOS_DMX_SEND) {
        aos_hal_mutex_lock(d->mx);
        aos_dmx_cfg_t c = d->cfg;
        aos_hal_mutex_unlock(d->mx);
        st->packets = (uint32_t)((now - s->t0_ms) / (c.period_ms ? c.period_ms : 25));
        st->slots = c.slots;
        st->start_code = c.start_code;
        st->break_us = c.break_us;
        st->mab_us = c.mab_us;
        st->age_ms = 0;
        return c.slots;
    }
    uint8_t u[513];
    int slots;
    uint32_t period;
    if (s_sender && s_sender != d) {
        aos_hal_mutex_lock(s_sender->mx);
        aos_dmx_cfg_t c = s_sender->cfg;
        memcpy(u, s_sender->tx, 513);
        aos_hal_mutex_unlock(s_sender->mx);
        u[0] = c.start_code;
        slots = c.slots;
        period = c.period_ms ? c.period_ms : 25;
        st->break_us = c.break_us;
        st->mab_us = c.mab_us;
    } else if (console_on()) {
        console_packet(u, (float)now / 1000.0f);
        slots = 512;
        period = 25;
        st->break_us = (now / 25) % 600 == 0 ? 60 : 176;     /* one short BREAK every 15 s */
        st->mab_us = 12;
    } else {
        st->age_ms = (uint32_t)(now - s->t0_ms);
        return 0;
    }
    uint32_t pk = (uint32_t)((now - s->t0_ms) / period);
    st->packets = pk;
    st->short_break = pk / 600;
    st->bad = 0;
    st->other_code = u[0] ? pk : 0;
    st->slots = (uint16_t)slots;
    st->start_code = u[0];
    st->age_ms = (uint32_t)((now - s->t0_ms) % period);
    int n = slots + 1 < max ? slots + 1 : max;
    if (out && n > 0) memcpy(out, u, (size_t)n);
    return slots;
}

void aos_io_be_dmx_close(aos_io_dmx_t *d)
{
    if (s_sender == d) s_sender = NULL;
    free(d->be);
    d->be = NULL;
}
