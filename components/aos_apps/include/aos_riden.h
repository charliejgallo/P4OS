/*
 * P4OS - the bench's power supply: a Riden RD60xx over Modbus RTU
 * (aos_riden.c), as a service.
 *
 * The Modbus app has its own Riden tab with its own worker; this is the one
 * the Banco and its logger use, so the supply can be read with the app
 * closed while a log runs. The link is the Modbus app's (its preferences
 * mb_rd_port, mb_rd_baud, mb_rd_unit): one place to say where the supply is.
 * Only one of the two can hold the port at a time; the other says who has it.
 *
 * The service runs while somebody wants it: aos_riden_keep() at least every
 * two seconds (the Banco's timer, the logger's thread), and it gives the port
 * back two seconds after the last one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     linked;            /* the port is open */
    bool     ok;                /* a reading arrived, the values below mean something */
    char     where[48];         /* "uart.a · 115200 · unidad 1" */
    char     err[96];
    uint16_t model;             /* 6012, 6006, 6018... */
    uint32_t serial;
    uint16_t fw;                /* 142 = 1.42 */
    int      idec;              /* decimals of the current: 3 on the 6006, 2 on the rest */
    float    v_set, i_set, v_out, i_out, p_out, v_in;
    bool     on, cc, locked;
    int      protect;           /* 0, 1 OVP, 2 OCP */
    int      temp_c;
    float    ah, wh;
    uint32_t t_ms;              /* uptime of the last reading */
    uint32_t seq;               /* bumps with every reading */
    char     wmsg[96];          /* the outcome of the last write, when it failed */
    uint32_t wseq;
} aos_riden_status_t;

enum { AOS_RIDEN_KEEP_UI = 1, AOS_RIDEN_KEEP_LOG = 2 };

void aos_riden_keep(int who);
void aos_riden_status(aos_riden_status_t *out);
/* Each write is read back: the supply refuses in silence what it does not
 * like, and wmsg says so. */
bool aos_riden_set_v(float v);
bool aos_riden_set_i(float a);
bool aos_riden_set_output(bool on);
/* The Modbus app changed the link: take the preferences again. */
void aos_riden_reload(void);

#ifdef __cplusplus
}
#endif
