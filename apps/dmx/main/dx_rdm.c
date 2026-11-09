/*
 * P4OS - DMX: RDM, the controller side (ANSI E1.20). The firmware sends a
 * request between two DMX packets and hands back the answer
 * (aos_io_dmx_rdm); this file builds the requests, reads the answers and
 * runs the jobs in a thread of its own, since each one waits for the line:
 *
 *   discovery  every fixture's UID without knowing any: un-mute them all,
 *              then ask "anybody between these two UIDs?" (DISC_UNIQUE_
 *              BRANCH) starting with the whole space. Nobody: done with
 *              that range. One clean answer: that UID; mute it (so it
 *              stops answering) and ask the same range again. Garbled
 *              answers (two or more at once): split the range in two.
 *              Then, for each one found, DEVICE_INFO (address, footprint,
 *              model) and its labels.
 *   actions    identify on and off, a new start address, a new label: a
 *              GET or SET to one UID.
 *
 * The link stays the timer's: while a job runs, the app does not close it
 * (dmx.c asks dx_rdm_busy()).
 */
#include "dx.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    CC_DISC = 0x10, CC_GET = 0x20, CC_SET = 0x30,
    PID_DISC_UNIQUE_BRANCH = 0x0001, PID_DISC_MUTE = 0x0002, PID_DISC_UN_MUTE = 0x0003,
    PID_DEVICE_INFO = 0x0060, PID_DEVICE_MODEL_DESCRIPTION = 0x0080, PID_MANUFACTURER_LABEL = 0x0081,
    PID_DEVICE_LABEL = 0x0082, PID_DMX_START_ADDRESS = 0x00F0, PID_IDENTIFY_DEVICE = 0x1000,
};

/* the controller's own UID: the prototype manufacturer range */
static const uint8_t ME[6] = { 0x7F, 0xF0, 0xFF, 0xFF, 0xFF, 0xF0 };
static const uint8_t ALL[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static volatile bool s_busy, s_stop;
static uint8_t s_tn;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void put48(uint8_t *p, uint64_t v)
{
    for (int i = 5; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

bool dx_rdm_busy(void) { return s_busy; }

/* One request; the answer's data into pd (when not NULL). Returns the
 * response type (0 ACK, 1 ACK_TIMER, 2 NACK, 3 overflow), -1 when nothing
 * came, -2 when what came is not a good answer. */
static int rdm_cmd(const uint8_t *dst, uint8_t cc, uint16_t pid, const uint8_t *data, int dl, uint8_t *pd, int *pdl)
{
    uint8_t q[260], r[300];
    q[0] = 0xCC;
    q[1] = 0x01;
    q[2] = (uint8_t)(24 + dl);
    memcpy(q + 3, dst, 6);
    memcpy(q + 9, ME, 6);
    q[15] = s_tn++;
    q[16] = 1;                              /* port id */
    q[17] = 0;
    q[18] = 0; q[19] = 0;                   /* root device */
    q[20] = cc;
    put16(q + 21, pid);
    q[23] = (uint8_t)dl;
    if (dl) memcpy(q + 24, data, (size_t)dl);
    uint16_t sum = 0;
    for (int i = 0; i < 24 + dl; i++) sum += q[i];
    put16(q + 24 + dl, sum);
    if (!DX.h) return -1;
    int n = aos_io_dmx_rdm(DX.h, q, 26 + dl, r, sizeof r, 3);
    DX.rdm_req++;
    if (n <= 0) return -1;
    if (n < 26 || r[0] != 0xCC || r[1] != 0x01 || r[2] < 24 || n < r[2] + 2) { DX.rdm_bad++; return -2; }
    sum = 0;
    for (int i = 0; i < r[2]; i++) sum += r[i];
    if (sum != get16(r + r[2]) || memcmp(r + 9, dst, 6) || r[20] != cc + 1 || get16(r + 21) != pid) { DX.rdm_bad++; return -2; }
    DX.rdm_ans++;
    int l = r[23];
    if (pd && pdl) { memcpy(pd, r + 24, (size_t)l); *pdl = l; }
    return r[16];
}

/* A discovery branch: 0 nobody, 1 one UID (in *uid), 2 a collision. */
static int branch(uint64_t lo, uint64_t hi, uint8_t *uid)
{
    uint8_t q[38], r[64];
    q[0] = 0xCC; q[1] = 0x01; q[2] = 36;
    memcpy(q + 3, ALL, 6);
    memcpy(q + 9, ME, 6);
    q[15] = s_tn++;
    q[16] = 1; q[17] = 0; q[18] = 0; q[19] = 0;
    q[20] = CC_DISC;
    put16(q + 21, PID_DISC_UNIQUE_BRANCH);
    q[23] = 12;
    put48(q + 24, lo);
    put48(q + 30, hi);
    uint16_t sum = 0;
    for (int i = 0; i < 36; i++) sum += q[i];
    put16(q + 36, sum);
    if (!DX.h) return 0;
    int n = aos_io_dmx_rdm(DX.h, q, 38, r, sizeof r, 3);
    DX.rdm_req++;
    if (n <= 0) return 0;
    /* up to 7 x 0xFE, then 0xAA, then 12 + 4 bytes */
    int k = 0;
    while (k < n && k < 8 && r[k] == 0xFE) k++;
    if (k >= n || r[k] != 0xAA || n - k - 1 < 16) { DX.rdm_coll++; return 2; }
    const uint8_t *e = r + k + 1;
    uint16_t s = 0;
    for (int i = 0; i < 12; i++) s += e[i];
    uint16_t cs = (uint16_t)((e[12] & e[13]) << 8 | (e[14] & e[15]));
    if (s != cs) { DX.rdm_coll++; return 2; }
    for (int i = 0; i < 6; i++) uid[i] = e[2 * i] & e[2 * i + 1];
    return 1;
}

static void read_info(dx_rdm_dev_t *dv)
{
    uint8_t pd[64];
    int l = 0;
    if (rdm_cmd(dv->uid, CC_GET, PID_DEVICE_INFO, NULL, 0, pd, &l) == 0 && l >= 19) {
        dv->model_id = get16(pd + 2);
        dv->footprint = get16(pd + 10);
        dv->addr = get16(pd + 14);
        dv->ok = true;
    }
    static const struct { uint16_t pid; size_t off; } LBL[3] = {
        { PID_DEVICE_LABEL, offsetof(dx_rdm_dev_t, label) },
        { PID_DEVICE_MODEL_DESCRIPTION, offsetof(dx_rdm_dev_t, model) },
        { PID_MANUFACTURER_LABEL, offsetof(dx_rdm_dev_t, maker) },
    };
    for (int k = 0; k < 3 && !s_stop; k++) {
        l = 0;
        char *dst = (char *)dv + LBL[k].off;
        if (rdm_cmd(dv->uid, CC_GET, LBL[k].pid, NULL, 0, pd, &l) == 0) {
            if (l > 32) l = 32;
            memcpy(dst, pd, (size_t)l);
            dst[l] = 0;
        }
    }
    l = 0;
    if (rdm_cmd(dv->uid, CC_GET, PID_IDENTIFY_DEVICE, NULL, 0, pd, &l) == 0 && l >= 1) dv->identify = pd[0];
}

static void discover(void)
{
    DX.rdm_n = 0;
    DX.rdm_coll = 0;
    snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("Buscando…"));
    uint8_t ctl[4];
    int l;
    rdm_cmd(ALL, CC_DISC, PID_DISC_UN_MUTE, NULL, 0, ctl, &l);       /* nobody answers a broadcast */
    struct { uint64_t lo, hi; } st[64];
    int sp = 0, steps = 0;
    st[sp].lo = 0;
    st[sp].hi = 0xFFFFFFFFFFFEull;
    sp++;
    while (sp > 0 && !s_stop && steps < 600) {
        uint64_t lo = st[sp - 1].lo, hi = st[sp - 1].hi;
        steps++;
        uint8_t uid[6];
        int r = branch(lo, hi, uid);
        if (r == 0) { sp--; continue; }
        if (r == 2) {
            sp--;
            if (lo == hi || sp > 60) continue;
            uint64_t mid = lo + (hi - lo) / 2;
            st[sp].lo = mid + 1; st[sp].hi = hi; sp++;
            st[sp].lo = lo; st[sp].hi = mid; sp++;
            continue;
        }
        /* one: mute it, and the same range again */
        bool known = false;
        for (int i = 0; i < DX.rdm_n; i++) if (!memcmp(DX.rdm_dev[i].uid, uid, 6)) known = true;
        int mr = rdm_cmd(uid, CC_DISC, PID_DISC_MUTE, NULL, 0, ctl, &l);
        if (!known && DX.rdm_n < DX_RDM_MAX) {
            dx_rdm_dev_t *dv = &DX.rdm_dev[DX.rdm_n];
            memset(dv, 0, sizeof *dv);
            memcpy(dv->uid, uid, 6);
            DX.rdm_n++;
            snprintf(DX.rdm_msg, sizeof DX.rdm_msg, _("Buscando… %d encontrados"), DX.rdm_n);
        }
        if (mr < 0) sp--;               /* it does not mute: do not ask it forever */
    }
    for (int i = 0; i < DX.rdm_n && !s_stop; i++) {
        snprintf(DX.rdm_msg, sizeof DX.rdm_msg, _("Leyendo %d de %d…"), i + 1, DX.rdm_n);
        read_info(&DX.rdm_dev[i]);
    }
    rdm_cmd(ALL, CC_DISC, PID_DISC_UN_MUTE, NULL, 0, ctl, &l);       /* leave them as they were */
    if (DX.rdm_n) snprintf(DX.rdm_msg, sizeof DX.rdm_msg, _("%d aparatos RDM"), DX.rdm_n);
    else snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("Ningún aparato contestó RDM."));
}

static void act(void)
{
    int i = DX.rdm_act_dev;
    if (i < 0 || i >= DX.rdm_n) return;
    dx_rdm_dev_t *dv = &DX.rdm_dev[i];
    uint8_t pd[40];
    int r = -1, l = 0;
    switch (DX.rdm_act) {
    case DX_RDM_IDENTIFY:
        pd[0] = dv->identify ? 0 : 1;
        r = rdm_cmd(dv->uid, CC_SET, PID_IDENTIFY_DEVICE, pd, 1, NULL, NULL);
        if (r == 0) dv->identify = pd[0];
        break;
    case DX_RDM_ADDRESS:
        put16(pd, (uint16_t)DX.rdm_act_val);
        r = rdm_cmd(dv->uid, CC_SET, PID_DMX_START_ADDRESS, pd, 2, NULL, NULL);
        if (r == 0) dv->addr = (uint16_t)DX.rdm_act_val;
        break;
    case DX_RDM_LABEL:
        l = (int)strlen(DX.rdm_act_text);
        if (l > 32) l = 32;
        memcpy(pd, DX.rdm_act_text, (size_t)l);
        r = rdm_cmd(dv->uid, CC_SET, PID_DEVICE_LABEL, pd, l, NULL, NULL);
        if (r == 0) snprintf(dv->label, sizeof dv->label, "%.32s", DX.rdm_act_text);
        break;
    default: return;
    }
    if (r == 0) snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("Hecho."));
    else if (r == 2) snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("El aparato no lo acepta (NACK)."));
    else snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("El aparato no contestó."));
}

static void job_main(void *arg)
{
    int job = (int)(intptr_t)arg;
    if (job == DX_RDM_DISCOVER) discover();
    else act();
    DX.rdm_seq++;
    s_busy = false;
}

bool dx_rdm_start(int job)
{
    if (s_busy || !DX.h || DX.dir != AOS_DMX_SEND) return false;
    s_busy = true;
    s_stop = false;
    if (!aos_hal_thread_start("dmx_rdm", job_main, (void *)(intptr_t)job, 6144, 4)) { s_busy = false; return false; }
    return true;
}

void dx_rdm_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 300 && s_busy; i++) aos_hal_sleep_ms(10);
}
