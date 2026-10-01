/*
 * P4OS - a Modbus master (aos_modbus.c): RTU over a UART port of the header
 * (through aos_io, RS485 when the port has a DE pin) and TCP over the network
 * (aos_hal_tcp_*).
 *
 * Synchronous on purpose: every call sends one request and waits for its
 * answer. Call it from a thread of your own, never from LVGL's task; one
 * thread per link. The functions return 0 when the slave answered, a Modbus
 * exception code (1..11) when it refused, or a negative AOS_MB_E_* for
 * everything that never reached an answer.
 *
 * The details that make real devices work:
 *   - a gap before each request (gap_ms): the Riden stops answering without
 *     ~20 ms of silence between frames;
 *   - retries of timeouts and bad CRCs only (an exception is an answer);
 *   - leftovers of a late answer are drained before the next request, so one
 *     slow reply never shifts every later one by a frame.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AOS_MB_RTU = 0, AOS_MB_TCP } aos_mb_transport_t;

typedef struct {
    aos_mb_transport_t transport;
    char     port[16];          /* RTU: "uart.a" */
    uint32_t baud;              /* RTU: 0 = 9600 */
    char     parity;            /* RTU: 'N' (0 too), 'E' or 'O'; 8 data bits always */
    uint8_t  stop_bits;         /* RTU: 1 (0 too) or 2 */
    char     host[64];          /* TCP */
    int      tcp_port;          /* TCP: 0 = 502 */
    uint8_t  unit;              /* slave / unit id: 0 = 1 */
    uint16_t timeout_ms;        /* per attempt: 0 = 300 (RTU) / 1000 (TCP) */
    uint16_t gap_ms;            /* silence before each request: 0 = 5 */
    uint8_t  retries;           /* 0 = 2 */
} aos_mb_link_t;

enum {
    AOS_MB_E_TIMEOUT = -1,      /* nobody answered */
    AOS_MB_E_CRC     = -2,      /* an answer, damaged */
    AOS_MB_E_FRAME   = -3,      /* an answer that does not fit the question */
    AOS_MB_E_IO      = -4,      /* the port or the connection failed */
    AOS_MB_E_ARG     = -5,
};

/* The function codes, for aos_mb_read. */
enum { AOS_MB_COILS = 1, AOS_MB_DISCRETE = 2, AOS_MB_HOLDING = 3, AOS_MB_INPUT = 4 };

typedef struct aos_mb aos_mb_t;

/* Opens the link (claims the UART for 'owner', or connects). NULL on
 * failure, with the reason in err. */
aos_mb_t *aos_mb_open(const aos_mb_link_t *link, const char *owner, char *err, size_t err_len);
void      aos_mb_close(aos_mb_t *m);
void      aos_mb_set_unit(aos_mb_t *m, uint8_t unit);

/* FC 1/2 read bits (one per out[] element, 0 or 1), FC 3/4 read registers.
 * count up to 125 registers or 2000 bits. */
int aos_mb_read(aos_mb_t *m, int fc, uint16_t addr, uint16_t count, uint16_t *out);
int aos_mb_write_coil(aos_mb_t *m, uint16_t addr, bool on);                         /* FC 5 */
int aos_mb_write_reg(aos_mb_t *m, uint16_t addr, uint16_t value);                    /* FC 6 */
int aos_mb_write_coils(aos_mb_t *m, uint16_t addr, uint16_t count, const uint8_t *bits);   /* FC 15 */
int aos_mb_write_regs(aos_mb_t *m, uint16_t addr, uint16_t count, const uint16_t *values); /* FC 16 */

/* For the user: "sin respuesta", "dirección inválida (02)"... */
const char *aos_mb_strerror(int rc);

typedef struct {
    uint32_t requests, answers, timeouts, crc_errors, exceptions;
    uint32_t last_ms;           /* round trip of the last answer */
} aos_mb_stats_t;
void aos_mb_stats(aos_mb_t *m, aos_mb_stats_t *out);

#ifdef __cplusplus
}
#endif
