/*
 * P4OS - Modbus: what the app's files share.
 *
 * A bench tool for Modbus RTU on RS485 (and RTU on any serial port, and the
 * master side of Modbus TCP). The app plays one role at a time on one link:
 *
 *   master    asks a slave: any function, once or over and over, and the
 *             tools around it (find the units on a bus, find a device's
 *             speed and format);
 *   slave     answers as a device would, with a register space the user
 *             edits, values that move by themselves, and faults on demand
 *             (silent, slow, bad CRC, exceptions) to see how a master copes;
 *   monitor   listens without ever driving the line, splits what goes by
 *             into frames, pairs questions and answers and keeps a table
 *             per unit: who answers, how fast, what fails.
 *
 * Every frame that goes by, in any role, lands in one log (the Traffic tab)
 * with its time, the silence before it and what it means.
 *
 * Threads: the bus thread (mb_bus.c) owns the link while it is open and is
 * the only one that touches the port; the UI (LVGL's thread) reads what it
 * left and asks for things under one mutex (mb_lock). Nobody holds the lock
 * across a call into the port.
 *
 * The protocol itself (mb_proto.c) is the standard's: Modbus over serial line
 * V1.02 (RTU: unit, PDU, CRC-16 0xA001 low byte first) and Modbus Messaging
 * on TCP/IP V1.0b (MBAP header, no CRC).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb_prof.h"

#define MB_MAXF   264           /* a whole RTU frame is 256 at most; a TCP ADU 260 */
#define MB_LOGN   1024          /* frames kept for the Traffic tab (PSRAM) */
#define MB_SPACE  10000         /* the slave's tables: addresses 0..9999 of each (PSRAM) */
#define MB_UNITS  32            /* units the monitor keeps a row for */

/* ---- the link ---- */

typedef enum { MB_TR_RTU = 0, MB_TR_TCP } mb_tr_t;

typedef struct {
    mb_tr_t  tr;
    char     port[16];          /* "uart.a", "usb0" */
    uint32_t baud;
    char     par;               /* 'N', 'E', 'O' */
    uint8_t  stop;              /* 1, 2 */
    char     host[64];
    uint16_t tport;
    uint16_t timeout_ms;        /* master: how long an answer may take */
    uint16_t gap_ms;            /* master: silence before each request */
} mb_cfg_t;

typedef enum { MB_ROLE_MASTER = 0, MB_ROLE_SLAVE, MB_ROLE_MONITOR, MB_ROLE_COUNT } mb_role_t;

/* ---- the log ---- */

enum {
    MB_LF_TX     = 0x01,        /* we sent it */
    MB_LF_CRC    = 0x02,        /* the CRC does not match */
    MB_LF_EXC    = 0x04,        /* an exception answer */
    MB_LF_NOISE  = 0x08,        /* bytes that are no frame */
    MB_LF_REQ    = 0x10,        /* a request (as far as can be told) */
    MB_LF_RSP    = 0x20,        /* an answer */
    MB_LF_NOTE   = 0x40,        /* not bytes: an event (no answer, a fault played) */
    MB_LF_TCP    = 0x80,        /* d[] is a TCP ADU (MBAP + PDU) */
};

enum { MB_NOTE_NONE = 0, MB_NOTE_TIMEOUT, MB_NOTE_SILENT, MB_NOTE_LATE, MB_NOTE_OTHER_UNIT, MB_NOTE_OPEN, MB_NOTE_CLOSE };

typedef struct {
    uint64_t t_us;              /* aos_hal_uptime_us() when it was seen */
    uint32_t gap_us;            /* the silence before it (RTU), as far as it can be measured */
    uint16_t len;
    uint8_t  fl;
    uint8_t  note;
    uint16_t rt_ms;             /* an answer: how long after its request */
    uint8_t  d[MB_MAXF];
} mb_log_t;

/* ---- the master ---- */

typedef struct {
    uint8_t  unit;
    uint8_t  fc;                /* 1-6, 8, 15, 16, 17, 43 */
    uint16_t addr, n;
    uint16_t wv[123];           /* the values a write sends (FC 5/15: 0 or 1) */
    uint16_t wn;
} mb_mreq_t;

enum { MB_TOOL_NONE = 0, MB_TOOL_SCAN, MB_TOOL_BAUD };

/* ---- the slave ---- */

enum { MB_FAULT_NONE = 0, MB_FAULT_SILENT, MB_FAULT_SLOW, MB_FAULT_CRC, MB_FAULT_BUSY, MB_FAULT_FAIL, MB_FAULT_COUNT };

/* ---- the monitor ---- */

typedef struct {
    uint8_t  unit;
    uint32_t req, rsp, timeouts, exc, crc;
    uint32_t rt_sum, rt_max, rt_n;
    uint8_t  last_fc;
    uint16_t last_addr, last_n;
    uint32_t last_ms;           /* uptime of its last request */
} mb_unit_t;

/* ---- the shared state ---- */

typedef struct {
    /* the link: set by the UI, read by the thread when it opens */
    mb_cfg_t  cfg;
    mb_role_t role;
    bool      want_open;        /* the UI asks */
    bool      open;             /* the thread says */
    char      err[96];
    char      desc[64];         /* "uart.a TX30 RX31 · RS485" */
    uint32_t  seq;              /* bumps when anything shown changes */

    /* the log: a ring in PSRAM */
    mb_log_t *log;
    uint32_t  log_seq;          /* entries ever written */
    uint64_t  t0_us;            /* times are shown from here */

    /* master */
    mb_mreq_t mreq;
    bool      m_once;           /* send it once, now */
    bool      m_live;           /* over and over */
    uint16_t  m_period_ms;
    int       m_rc;             /* 0, an exception code, or MB_E_* */
    uint8_t   m_got_fc;
    uint16_t  m_got_addr, m_got_n;
    uint16_t  m_val[2000];      /* registers, or one bit per element */
    char      m_text[192];      /* FC 17 / 43: what the device says of itself */
    uint32_t  m_seq;
    uint32_t  m_req, m_ans, m_tmo, m_crc, m_exc;
    uint32_t  m_rt_last, m_rt_sum, m_rt_max;
    /* writes queued by the grid (a tap on a coil or a register) */
    mb_mreq_t m_wq[8];
    int       m_wqn;
    /* the tools */
    int       tool;
    bool      tool_stop;
    int       tool_pos, tool_total;
    uint8_t   found[248];       /* scan: 1 = answered, 2 = refused (exception) */
    char      bauds_found[8][24];
    int       nbauds_found;
    uint32_t  tool_seq;

    /* slave */
    uint8_t   s_unit;
    int       s_fault;
    uint16_t  s_delay_ms;       /* the slow fault's delay */
    bool      s_anim;           /* inputs that move by themselves */
    uint8_t  *s_coil, *s_di;    /* MB_SPACE each (PSRAM) */
    uint16_t *s_hr, *s_ir;
    uint32_t  s_req, s_mine, s_other, s_bcast, s_crc, s_exc;
    char      s_last[96];
    uint32_t  s_seq;

    /* the device profile (mb_prof.h), shared by the master and the slave:
     * the UI loads it and bumps prof_gen, the thread takes a copy */
    mb_prof_t prof;
    bool      prof_on;
    uint32_t  prof_gen;
    double    pv[MB_PTS];       /* each point's value: read (master) or played (slave) */
    int       prc[MB_PTS];      /* master: 0, or what its read got */
    uint32_t  pv_seq;

    /* monitor */
    mb_unit_t units[MB_UNITS];
    int       nunits;
    uint32_t  mon_frames, mon_bytes, mon_crc, mon_noise;
} mb_shared_t;

extern mb_shared_t MB;

void mb_lock(void);
void mb_unlock(void);

/* mb_bus.c */
bool mb_bus_init(void);         /* allocates and starts the thread */
void mb_bus_deinit(void);       /* stops it and closes the link */
void mb_bus_clear_log(void);
void mb_bus_clear_stats(void);
/* sets a profile point in the slave's tables (the UI's tap) */
void mb_bus_point_put(const mb_point_t *pt, double v);
/* copies the n-th newest entry (0 = the newest); false when there is none */
bool mb_log_get(uint32_t index_from_seq, mb_log_t *out);

/* ---- mb_proto.c ---- */

enum { MB_E_TIMEOUT = -1, MB_E_CRC = -2, MB_E_FRAME = -3, MB_E_IO = -4, MB_E_ARG = -5 };

uint16_t mb_crc16(const uint8_t *p, size_t n);
bool     mb_crc_ok(const uint8_t *f, int len);
void     mb_crc_put(uint8_t *f, int len);           /* appends to f[len], f[len+1] */
/* Builds a request PDU (no unit, no CRC) into out; its length or -1. */
int      mb_build_pdu(const mb_mreq_t *q, uint8_t *out);
/* The lengths an RTU frame starting with these bytes may have: up to 3, in
 * the order worth trying; 0 when more bytes are needed to know. */
int      mb_rtu_lengths(const uint8_t *f, int have, int *out);
/* The length of a normal answer to the request PDU q, with unit and CRC, or
 * -1 when the answer says it (a byte count at [2]). */
int      mb_rtu_answer_len(const uint8_t *q);
/* One line saying what a frame (unit + PDU, no CRC needed) means. */
void     mb_describe(const uint8_t *f, int len, bool is_req, char *out, size_t n);
const char *mb_fc_name(int fc);
const char *mb_strerror(int rc);
/* The time of 'chars' characters at this speed and format, in us. */
uint32_t mb_char_us(const mb_cfg_t *c, float chars);
