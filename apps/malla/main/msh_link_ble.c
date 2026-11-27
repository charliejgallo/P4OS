/*
 * Malla - the Bluetooth link.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Meshtastic's GATT service (6ba1b218-15a8-461f-9fa8-5dcae273eafd) has three
 * characteristics that matter to a client:
 *
 *   ToRadio   f75c76d2-129e-4dad-a1dd-7866124401e7  write: one ToRadio
 *   FromRadio 2c55e69e-4993-11ed-b878-0242ac120002  read: one FromRadio, or
 *                                                   nothing when drained
 *   FromNum   ed9da18b-a800-4f66-a670-aa7547e34453  notify: "there is more"
 *
 * So a client writes, then reads FromRadio until a read comes back empty,
 * and does it again every time FromNum notifies. The HAL's GATT client
 * (aos_hal_ble_gatt_*) queues every operation and answers through its
 * event ring; this file turns that into the link's recv/send.
 *
 * The node asks for an encrypted, bonded link before it lets anything be
 * read (its bluetooth.mode: a random PIN on its screen, a fixed one, 123456
 * unless changed, or none). With the firmware's pairing API
 * (aos_hal_ble_gatt_pair & co., MSH_BLE_PAIR, set by the build when
 * aos_hal.h has them) the link asks for security right after discovery and,
 * when the node wants a PIN, hands the question to the person through
 * l->ask. Without it, a node in "no PIN" mode works and any other answers
 * with ATT error 0x105 or 0x10F, which is said in words.
 */
#include "msh_link.h"
#include "msh_util.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EV_MAX     4
#define ATTRS_MAX  96

/* The UUIDs as they are written; turned into the little-endian bytes the HAL
 * gives with uuid_le(). */
static const char UUID_SERVICE[] = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char UUID_TORADIO[] = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char UUID_FROMRADIO[] = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char UUID_FROMNUM[] = "ed9da18b-a800-4f66-a670-aa7547e34453";

typedef struct {
    uint16_t h_to, h_from, h_num;
    bool reading;                   /* a read of FromRadio is out */
    bool more;                      /* FromNum said so, or we just wrote */
    uint32_t read_t;                /* when the read went out */
    aos_ble_gatt_ev_t ev[EV_MAX];
    int ev_n, ev_i;
    aos_ble_attr_t at[ATTRS_MAX];
} ble_t;

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void uuid_le(const char *s, uint8_t out[16])
{
    uint8_t be[16];
    int n = 0;
    for (; *s && n < 16; s++) {
        if (*s == '-') continue;
        be[n++] = (uint8_t)(hexv(s[0]) << 4 | hexv(s[1]));
        s++;
    }
    for (int i = 0; i < 16; i++) out[i] = be[15 - i];
}

static bool uuid_is(const aos_ble_attr_t *a, const char *s)
{
    uint8_t u[16];
    uuid_le(s, u);
    return a->uuid_len == 16 && !memcmp(a->uuid, u, 16);
}

bool msh_ble_adv_is_meshtastic(const uint8_t *data, int len)
{
    /* the 128-bit service UUID in the advertisement or the scan response
     * (AD types 0x06 incomplete, 0x07 complete list) */
    uint8_t u[16];
    uuid_le(UUID_SERVICE, u);
    for (int i = 0; i + 1 < len;) {
        int l = data[i];
        if (l == 0 || i + 1 + l > len) break;
        int type = data[i + 1];
        if (type == 0x06 || type == 0x07)
            for (int k = i + 2; k + 16 <= i + 1 + l; k += 16)
                if (!memcmp(data + k, u, 16)) return true;
        i += 1 + l;
    }
    return false;
}

/* Waits for the GATT client to leave CONNECTING/DISCOVERING. */
static aos_ble_gatt_state_t wait_ready(msh_link_t *l, int ms, int *reason)
{
    aos_ble_gatt_state_t st = AOS_BLE_GATT_IDLE;
    for (int t = 0; t < ms && !l->cancel; t += 50) {
        st = aos_hal_ble_gatt_state(reason);
        if (st == AOS_BLE_GATT_READY || st == AOS_BLE_GATT_FAILED || st == AOS_BLE_GATT_IDLE) return st;
        aos_hal_sleep_ms(50);
    }
    return st;
}

/* The next event, from the few fetched at a time. */
static aos_ble_gatt_ev_t *next_ev(ble_t *b)
{
    if (b->ev_i >= b->ev_n) {
        b->ev_n = aos_hal_ble_gatt_events(b->ev, EV_MAX);
        b->ev_i = 0;
        if (b->ev_n <= 0) { b->ev_n = 0; return NULL; }
    }
    return &b->ev[b->ev_i++];
}

#if MSH_BLE_PAIR
/* Asks for an encrypted link and answers the PIN when the node wants one.
 * Events that are not about security are dropped meanwhile: nothing else
 * is out yet. */
static bool pair(msh_link_t *l, ble_t *b, char *err, size_t err_len)
{
    if (aos_hal_ble_gatt_secure()) return true;
    l->note = N_("Emparejando…");
    if (!aos_hal_ble_gatt_pair()) {
        snprintf(err, err_len, "%s", N_("No se pudo pedir el emparejamiento"));
        return false;
    }
    for (int t = 0; t < 90000 && !l->cancel; t += 20) {
        aos_ble_gatt_ev_t *e = next_ev(b);
        if (!e) {
            int reason;
            aos_ble_gatt_state_t st = aos_hal_ble_gatt_state(&reason);
            if (st != AOS_BLE_GATT_READY) {
                snprintf(err, err_len, "%s", N_("Se cortó mientras emparejaba"));
                return false;
            }
            if (l->ask == MSH_ASK_PIN && l->answered) {
                l->ask = MSH_ASK_NONE;
                l->answered = false;
                aos_hal_ble_gatt_passkey(l->pin);
                l->note = N_("Emparejando…");
            }
            aos_hal_sleep_ms(20);
            continue;
        }
        if (e->type == AOS_BLE_EV_PASSKEY) {
            l->answered = false;
            l->ask = MSH_ASK_PIN;
            l->note = N_("El nodo pide el PIN");
            t = 0;                  /* the person gets the whole time to type it */
        } else if (e->type == AOS_BLE_EV_SECURITY) {
            l->ask = MSH_ASK_NONE;
            if (e->status == 0) return true;
            snprintf(err, err_len, "%s", N_("El emparejamiento falló (¿PIN equivocado?)"));
            return false;
        }
    }
    l->ask = MSH_ASK_NONE;
    snprintf(err, err_len, "%s", N_("El emparejamiento no terminó a tiempo"));
    return false;
}
#endif

static bool ble_open(msh_link_t *l, char *err, size_t err_len)
{
    if (!aos_hal_bt_enabled()) {
        snprintf(err, err_len, "%s", N_("El Bluetooth está apagado"));
        return false;
    }
    if (!l->priv) l->priv = msh_alloc(sizeof(ble_t));
    ble_t *b = (ble_t *)l->priv;
    if (!b) { snprintf(err, err_len, "%s", N_("Sin memoria")); return false; }
    memset(b, 0, sizeof *b);
    aos_ble_gatt_ev_t drop[EV_MAX];
    while (aos_hal_ble_gatt_events(drop, EV_MAX) > 0) {}     /* an old connection's leftovers */
    l->note = N_("Conectando por Bluetooth…");
    if (!aos_hal_ble_gatt_connect(l->t.addr, l->t.addr_type)) {
        snprintf(err, err_len, "%s", N_("El Bluetooth no aceptó la conexión"));
        return false;
    }
    int reason = 0;
    aos_ble_gatt_state_t st = wait_ready(l, 45000, &reason);
    if (st != AOS_BLE_GATT_READY) {
        aos_hal_ble_gatt_disconnect();
        snprintf(err, err_len, "%s", N_("El nodo no aceptó la conexión"));
        return false;
    }
    int n = aos_hal_ble_gatt_attrs(b->at, ATTRS_MAX);
    bool in_svc = false;
    for (int i = 0; i < n; i++) {
        const aos_ble_attr_t *a = &b->at[i];
        if (a->kind == AOS_BLE_ATTR_SERVICE) { in_svc = uuid_is(a, UUID_SERVICE); continue; }
        if (!in_svc || a->kind != AOS_BLE_ATTR_CHAR) continue;
        if (uuid_is(a, UUID_TORADIO)) b->h_to = a->handle;
        else if (uuid_is(a, UUID_FROMRADIO)) b->h_from = a->handle;
        else if (uuid_is(a, UUID_FROMNUM)) b->h_num = a->handle;
    }
    if (!b->h_to || !b->h_from) {
        aos_hal_ble_gatt_disconnect();
        snprintf(err, err_len, "%s", N_("No es un nodo Meshtastic"));
        return false;
    }
#if MSH_BLE_PAIR
    if (!pair(l, b, err, err_len)) {
        aos_hal_ble_gatt_disconnect();
        return false;
    }
#endif
    if (b->h_num) aos_hal_ble_gatt_subscribe(b->h_num, 1);
    l->note = "";
    return true;
}

static int ble_recv(msh_link_t *l, uint8_t *buf, int max, int timeout_ms, char *err, size_t err_len)
{
    ble_t *b = (ble_t *)l->priv;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    for (;;) {
        int reason = 0;
        if (aos_hal_ble_gatt_state(&reason) != AOS_BLE_GATT_READY) {
            snprintf(err, err_len, "%s", N_("Se cortó el Bluetooth"));
            return -1;
        }
        if (b->more && !b->reading) {
            if (aos_hal_ble_gatt_read(b->h_from)) {
                b->reading = true;
                b->read_t = (uint32_t)aos_hal_uptime_ms();
            }
        }
        aos_ble_gatt_ev_t *e = next_ev(b);
        if (e) {
            if (e->type == AOS_BLE_EV_NOTIFY && e->handle == b->h_num) {
                b->more = true;
            } else if (e->type == AOS_BLE_EV_READ && e->handle == b->h_from) {
                b->reading = false;
                if (e->status == 0x105 || e->status == 0x10F) {
                    snprintf(err, err_len, "%s", N_("El nodo pide emparejar con PIN"));
                    return -1;
                }
                if (e->status != 0 || e->len == 0) {
                    b->more = false;        /* drained (or the read failed: FromNum will say) */
                } else {
                    int n = e->len > max ? max : e->len;
                    memcpy(buf, e->data, (size_t)n);
                    return n;               /* and more stays set: read again */
                }
            }
            continue;
        }
        /* a read that never came back (a dropped event): ask again */
        if (b->reading && (uint32_t)aos_hal_uptime_ms() - b->read_t > 5000) b->reading = false;
        if ((int)((uint32_t)aos_hal_uptime_ms() - t0) >= timeout_ms) return 0;
        aos_hal_sleep_ms(10);
    }
}

static bool ble_send(msh_link_t *l, const uint8_t *buf, int len)
{
    ble_t *b = (ble_t *)l->priv;
    if (!b || !b->h_to) return false;
    bool ok = aos_hal_ble_gatt_write(b->h_to, buf, (size_t)len, true);
    b->more = true;                 /* an answer may be waiting already */
    return ok;
}

static void ble_close(msh_link_t *l)
{
    ble_t *b = (ble_t *)l->priv;
    if (b && b->h_to) aos_hal_ble_gatt_disconnect();
    if (b) b->h_to = 0;
}

const msh_link_ops_t msh_link_ble_ops = { ble_open, ble_recv, ble_send, ble_close };
