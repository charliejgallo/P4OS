/* An RDM responder (ANSI E1.20), the fixture side, with no platform in it:
 * the bench's ESP32 uses it in the DMX light role, and the P4OS simulator
 * builds the same file for its make-believe fixtures (sim/io_dmx_sim.c).
 *
 * RDM rides on the DMX line: the controller sends a packet with start code
 * 0xCC instead of 0, and the fixture answers on the same pair within 2 ms.
 * A packet: 0xCC, 0x01 (sub start code), its length (from 0xCC to the end
 * of the data), the destination UID (6 bytes: manufacturer and device),
 * the source UID, a transaction number, the port id (in an answer, the
 * response type: 0 ACK, 2 NACK with a reason), a message count, the
 * sub-device (2 bytes), the command class (0x10 discovery, 0x20 GET, 0x30
 * SET; the answer's is one more), the parameter id (2 bytes), the data
 * length and the data, and a 16-bit checksum: the sum of every byte before
 * it.
 *
 * Discovery finds the UIDs on a line without knowing any: the controller
 * asks "anybody between these two UIDs?" (DISC_UNIQUE_BRANCH) and every
 * fixture in the range that is not muted answers at once, with no BREAK,
 * a preamble (0xFE), 0xAA and its UID with each byte sent twice (b | 0xAA,
 * b | 0x55) and a checksum the same way. One answer decodes; two or more
 * collide into bytes whose checksum fails, and the controller splits the
 * range. A fixture found is muted (DISC_MUTE) so it stops answering.
 *
 * What this one answers: the three discovery messages, DEVICE_INFO,
 * SUPPORTED_PARAMETERS, DEVICE_MODEL_DESCRIPTION, MANUFACTURER_LABEL,
 * SOFTWARE_VERSION_LABEL, DEVICE_LABEL (GET and SET), DMX_START_ADDRESS
 * (GET and SET) and IDENTIFY_DEVICE (GET and SET); anything else gets a
 * NACK "unknown PID". */
#include "rdm_resp.h"

#include <string.h>

enum {
    CC_DISC = 0x10, CC_GET = 0x20, CC_SET = 0x30,
    PID_DISC_UNIQUE_BRANCH = 0x0001, PID_DISC_MUTE = 0x0002, PID_DISC_UN_MUTE = 0x0003,
    PID_SUPPORTED_PARAMETERS = 0x0050, PID_DEVICE_INFO = 0x0060, PID_DEVICE_MODEL_DESCRIPTION = 0x0080,
    PID_MANUFACTURER_LABEL = 0x0081, PID_DEVICE_LABEL = 0x0082, PID_SOFTWARE_VERSION_LABEL = 0x00C0,
    PID_DMX_START_ADDRESS = 0x00F0, PID_IDENTIFY_DEVICE = 0x1000,
    NR_UNKNOWN_PID = 0x0000, NR_FORMAT_ERROR = 0x0001, NR_DATA_OUT_OF_RANGE = 0x0006, NR_UNSUPPORTED_CC = 0x0005,
};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint64_t uid48(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = v << 8 | p[i];
    return v;
}

static bool for_me(const rdm_dev_t *dv, const uint8_t *dst, bool *broadcast)
{
    static const uint8_t ALL[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    *broadcast = false;
    if (!memcmp(dst, dv->uid, 6)) return true;
    if (!memcmp(dst, ALL, 6) || (!memcmp(dst, dv->uid, 2) && !memcmp(dst + 2, ALL + 2, 4))) { *broadcast = true; return true; }
    return false;
}

/* A normal answer: header from the request, then pd, then the checksum. */
static int answer(const rdm_dev_t *dv, const uint8_t *q, uint8_t type, const uint8_t *pd, int pdl, uint8_t *out)
{
    out[0] = 0xCC;
    out[1] = 0x01;
    out[2] = (uint8_t)(24 + pdl);
    memcpy(out + 3, q + 9, 6);              /* to whoever asked */
    memcpy(out + 9, dv->uid, 6);
    out[15] = q[15];                        /* transaction number */
    out[16] = type;
    out[17] = 0;                            /* message count */
    out[18] = q[18]; out[19] = q[19];       /* sub-device */
    out[20] = (uint8_t)(q[20] + 1);         /* command class: the response's */
    out[21] = q[21]; out[22] = q[22];       /* the parameter */
    out[23] = (uint8_t)pdl;
    if (pdl) memcpy(out + 24, pd, (size_t)pdl);
    uint16_t sum = 0;
    for (int i = 0; i < 24 + pdl; i++) sum += out[i];
    put16(out + 24 + pdl, sum);
    return 24 + pdl + 2;
}

static int nack(const rdm_dev_t *dv, const uint8_t *q, uint16_t reason, uint8_t *out)
{
    uint8_t pd[2];
    put16(pd, reason);
    return answer(dv, q, 2, pd, 2, out);
}

static int label_pd(const char *s, uint8_t *pd)
{
    int n = (int)strlen(s);
    if (n > 32) n = 32;
    memcpy(pd, s, (size_t)n);
    return n;
}

int rdm_handle(rdm_dev_t *dv, const uint8_t *q, int len, uint8_t *out, bool *with_break)
{
    *with_break = true;
    if (len < 26 || q[0] != 0xCC || q[1] != 0x01 || q[2] < 24 || len < q[2] + 2) return 0;
    uint16_t sum = 0;
    for (int i = 0; i < q[2]; i++) sum += q[i];
    if (sum != get16(q + q[2])) return 0;
    if (q[20] & 1) return 0;                 /* an answer (our own, echoed): not a question */
    bool bc;
    if (!for_me(dv, q + 3, &bc)) return 0;
    uint8_t cc = q[20];
    uint16_t pid = get16(q + 21);
    int pdl = q[23];
    const uint8_t *pd = q + 24;
    uint8_t r[64];
    int n = 0;

    if (cc == CC_DISC) {
        if (pid == PID_DISC_UNIQUE_BRANCH) {
            if (dv->muted || pdl != 12) return 0;
            uint64_t me = uid48(dv->uid), lo = uid48(pd), hi = uid48(pd + 6);
            if (me < lo || me > hi) return 0;
            /* no BREAK: 7 of 0xFE, 0xAA, the UID twice-encoded, the checksum */
            int k = 0;
            for (int i = 0; i < 7; i++) out[k++] = 0xFE;
            out[k++] = 0xAA;
            uint16_t s = 0;
            for (int i = 0; i < 6; i++) {
                out[k] = (uint8_t)(dv->uid[i] | 0xAA); s += out[k++];
                out[k] = (uint8_t)(dv->uid[i] | 0x55); s += out[k++];
            }
            out[k++] = (uint8_t)((s >> 8) | 0xAA);
            out[k++] = (uint8_t)((s >> 8) | 0x55);
            out[k++] = (uint8_t)((s & 0xFF) | 0xAA);
            out[k++] = (uint8_t)((s & 0xFF) | 0x55);
            *with_break = false;
            return k;
        }
        if (pid == PID_DISC_MUTE || pid == PID_DISC_UN_MUTE) {
            dv->muted = pid == PID_DISC_MUTE;
            if (bc) return 0;
            uint8_t ctl[2] = { 0, 0 };
            return answer(dv, q, 0, ctl, 2, out);
        }
        return 0;
    }
    if (cc != CC_GET && cc != CC_SET) return bc ? 0 : nack(dv, q, NR_UNSUPPORTED_CC, out);

    switch (pid) {
    case PID_DEVICE_INFO:
        if (cc != CC_GET) break;
        put16(r, 0x0100);                    /* RDM 1.0 */
        put16(r + 2, dv->model_id);
        put16(r + 4, 0x0101);                /* category: fixture */
        r[6] = 0; r[7] = 0; r[8] = 0; r[9] = 1;     /* software version id */
        put16(r + 10, dv->footprint);
        r[12] = 1; r[13] = 1;                /* personality 1 of 1 */
        put16(r + 14, dv->addr);
        put16(r + 16, 0);                    /* sub-devices */
        r[18] = 0;                           /* sensors */
        n = 19;
        break;
    case PID_SUPPORTED_PARAMETERS: {
        static const uint16_t P[] = { PID_DEVICE_MODEL_DESCRIPTION, PID_MANUFACTURER_LABEL, PID_DEVICE_LABEL, PID_SOFTWARE_VERSION_LABEL };
        if (cc != CC_GET) break;
        for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) put16(r + 2 * i, P[i]);
        n = (int)(2 * (sizeof P / sizeof P[0]));
        break;
    }
    case PID_DEVICE_MODEL_DESCRIPTION: if (cc == CC_GET) n = label_pd(dv->model, r); else goto unsupported; break;
    case PID_MANUFACTURER_LABEL: if (cc == CC_GET) n = label_pd(dv->maker, r); else goto unsupported; break;
    case PID_SOFTWARE_VERSION_LABEL: if (cc == CC_GET) n = label_pd(dv->version, r); else goto unsupported; break;
    case PID_DEVICE_LABEL:
        if (cc == CC_GET) { n = label_pd(dv->label, r); break; }
        if (pdl > 32) return bc ? 0 : nack(dv, q, NR_FORMAT_ERROR, out);
        memcpy(dv->label, pd, (size_t)pdl);
        dv->label[pdl] = 0;
        dv->changed = true;
        n = 0;
        break;
    case PID_DMX_START_ADDRESS:
        if (cc == CC_GET) { put16(r, dv->addr); n = 2; break; }
        if (pdl != 2) return bc ? 0 : nack(dv, q, NR_FORMAT_ERROR, out);
        if (get16(pd) < 1 || get16(pd) + dv->footprint - 1 > 512) return bc ? 0 : nack(dv, q, NR_DATA_OUT_OF_RANGE, out);
        dv->addr = get16(pd);
        dv->changed = true;
        n = 0;
        break;
    case PID_IDENTIFY_DEVICE:
        if (cc == CC_GET) { r[0] = dv->identify; n = 1; break; }
        if (pdl != 1 || pd[0] > 1) return bc ? 0 : nack(dv, q, NR_FORMAT_ERROR, out);
        dv->identify = pd[0];
        dv->changed = true;
        n = 0;
        break;
    default:
        return bc ? 0 : nack(dv, q, NR_UNKNOWN_PID, out);
    }
    if (bc) return 0;                        /* a broadcast is done, never answered */
    return answer(dv, q, 0, r, n, out);
unsupported:
    return bc ? 0 : nack(dv, q, NR_UNSUPPORTED_CC, out);
}
