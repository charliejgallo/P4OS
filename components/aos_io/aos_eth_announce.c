/*
 * P4OS - Ethernet: reading what a switch says of itself (aos_eth.h).
 *
 * A managed switch announces, on every port and every thirty seconds or so,
 * who it is and which of its ports this is: LLDP (IEEE 802.1AB, from
 * anyone) or CDP (Cisco's own, older). Both go to a group address that no
 * switch forwards, so whatever arrives was said by the switch this very
 * cable goes to. This file only reads a frame; who gets the frames is the
 * platform's business (aos_eth_p4.c, sim/eth_sim.c).
 *
 * Nothing here trusts the frame: every length is checked against what is
 * left of it, and every text is cut to its field and cleaned of what does
 * not print.
 */
#include "aos_eth.h"

#include <stdio.h>
#include <string.h>

static const uint8_t LLDP_DST[3][6] = {
    { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E },     /* nearest bridge: the usual one */
    { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x03 },     /* nearest non-TPMR bridge */
    { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x00 },     /* nearest customer bridge */
};
static const uint8_t CDP_DST[6] = { 0x01, 0x00, 0x0C, 0xCC, 0xCC, 0xCC };

/* A field of the frame as text: printable ASCII kept, the rest a dot. */
static void text(char *out, size_t n, const uint8_t *p, size_t len)
{
    size_t k = 0;
    while (len && (p[len - 1] == 0 || p[len - 1] == ' ' || p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    for (size_t i = 0; i < len && k + 1 < n; i++) {
        uint8_t c = p[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out[k++] = c >= 32 && c < 127 ? (char)c : '.';
    }
    out[k] = 0;
}

static void mac_text(char *out, size_t n, const uint8_t *m)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* A chassis or port identifier: its first byte says what the rest is. */
static void lldp_id(char *out, size_t n, const uint8_t *p, size_t len, bool chassis)
{
    if (len < 2) { out[0] = 0; return; }
    uint8_t sub = p[0];
    bool is_mac = chassis ? sub == 4 : sub == 3;
    bool is_addr = chassis ? sub == 5 : sub == 4;
    if (is_mac && len == 7) mac_text(out, n, p + 1);
    else if (is_addr && len == 6 && p[1] == 1) snprintf(out, n, "%u.%u.%u.%u", p[2], p[3], p[4], p[5]);
    else text(out, n, p + 1, len - 1);
}

static bool lldp(const uint8_t *p, size_t len, aos_eth_neighbor_t *o)
{
    bool end = false, any = false;
    while (len >= 2 && !end) {
        unsigned type = p[0] >> 1;
        size_t n = (size_t)(p[0] & 1) << 8 | p[1];
        p += 2;
        len -= 2;
        if (n > len) return false;              /* a TLV longer than the frame: not to be believed */
        switch (type) {
        case 0: end = true; break;
        case 1: lldp_id(o->chassis, sizeof o->chassis, p, n, true); any = true; break;
        case 2: lldp_id(o->port, sizeof o->port, p, n, false); any = true; break;
        case 3: if (n >= 2) o->ttl_s = (uint32_t)p[0] << 8 | p[1]; break;
        case 4: text(o->port_desc, sizeof o->port_desc, p, n); break;
        case 5: text(o->system, sizeof o->system, p, n); break;
        case 6: text(o->descr, sizeof o->descr, p, n); break;
        case 8:                                 /* management address: length, kind, the address */
            if (n >= 6 && p[0] == 5 && p[1] == 1)
                snprintf(o->mgmt, sizeof o->mgmt, "%u.%u.%u.%u", p[2], p[3], p[4], p[5]);
            break;
        case 127:                               /* an organisation's own: three bytes of who, one of what */
            if (n >= 6 && p[0] == 0x00 && p[1] == 0x80 && p[2] == 0xC2 && p[3] == 1)
                o->vlan = p[4] << 8 | p[5];     /* 802.1: the port's untagged VLAN */
            else if (n >= 5 && p[0] == 0x00 && p[1] == 0x12 && p[2] == 0x0F && p[3] == 2)
                o->poe = (p[4] & 0x03) == 0x03 ? 1 : 0;     /* 802.3 power: a source, and it is switched on */
            else if (n >= 5 && p[0] == 0x00 && p[1] == 0x12 && p[2] == 0xBB && p[3] == 4 && o->poe < 0)
                o->poe = (p[4] >> 6) == 0 ? 1 : 0;          /* LLDP-MED power: the sender is the source */
            break;
        default: break;
        }
        p += n;
        len -= n;
    }
    return any;
}

static bool cdp(const uint8_t *p, size_t len, aos_eth_neighbor_t *o)
{
    if (len < 4) return false;
    o->ttl_s = p[1];
    p += 4;                                     /* version, time to live, checksum */
    len -= 4;
    bool any = false;
    while (len >= 4) {
        unsigned type = p[0] << 8 | p[1];
        size_t n = (size_t)p[2] << 8 | p[3];    /* the whole TLV, its four bytes of header too */
        if (n < 4 || n > len) return false;
        const uint8_t *v = p + 4;
        size_t vn = n - 4;
        switch (type) {
        case 0x0001: text(o->system, sizeof o->system, v, vn); any = true; break;
        case 0x0003: text(o->port, sizeof o->port, v, vn); any = true; break;
        case 0x0005: text(o->descr, sizeof o->descr, v, vn); break;
        case 0x0006:                            /* the platform says more in less than the software's banner */
            if (vn) text(o->descr, sizeof o->descr, v, vn);
            break;
        case 0x000A: if (vn >= 2) o->vlan = v[0] << 8 | v[1]; break;
        case 0x0002:                            /* addresses: a count, then each with its protocol */
            if (vn >= 13 && v[4] == 1 && v[5] == 1 && v[6] == 0xCC && v[7] == 0 && v[8] == 4)
                snprintf(o->mgmt, sizeof o->mgmt, "%u.%u.%u.%u", v[9], v[10], v[11], v[12]);
            break;
        default: break;
        }
        p += n;
        len -= n;
    }
    return any;
}

bool aos_eth_parse_announce(const uint8_t *f, size_t len, aos_eth_neighbor_t *out)
{
    if (!f || !out || len < 22) return false;
    aos_eth_neighbor_t o;
    memset(&o, 0, sizeof o);
    o.poe = -1;
    size_t at = 12;                             /* past the two addresses */
    if (f[at] == 0x81 && f[at + 1] == 0x00) at += 4;       /* a VLAN tag before the type */
    bool is_lldp = false;
    for (int i = 0; i < 3; i++) is_lldp |= !memcmp(f, LLDP_DST[i], 6);
    if (is_lldp && f[at] == 0x88 && f[at + 1] == 0xCC) {
        if (!lldp(f + at + 2, len - at - 2, &o)) return false;
        snprintf(o.proto, sizeof o.proto, "LLDP");
    } else if (!memcmp(f, CDP_DST, 6) && len >= at + 10 && f[at + 2] == 0xAA && f[at + 3] == 0xAA && f[at + 4] == 0x03
               && f[at + 5] == 0x00 && f[at + 6] == 0x00 && f[at + 7] == 0x0C && f[at + 8] == 0x20 && f[at + 9] == 0x00) {
        if (!cdp(f + at + 10, len - at - 10, &o)) return false;        /* a length, LLC and SNAP, then CDP */
        snprintf(o.proto, sizeof o.proto, "CDP");
        if (!o.chassis[0]) mac_text(o.chassis, sizeof o.chassis, f + 6);
    } else {
        return false;
    }
    *out = o;
    return true;
}
