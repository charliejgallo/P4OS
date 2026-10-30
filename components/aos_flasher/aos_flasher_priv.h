/*
 * P4OS - Programmer: what the service's core (aos_flasher.c) gives the
 * families that talk to a target (aos_flasher_esp.c, and those to come:
 * SWD, AVR, STM32 - docs/plan/PROGRAMADOR.md).
 *
 * The core owns the job: its thread, the status the app and the portal read,
 * the log, cancelling, and the dump file. A family is one function that runs
 * a whole job on its port - opens it, does what J.job asks, leaves the
 * target running and closes the port - reporting through these calls. It
 * runs on the job's thread; everything here takes the core's lock itself,
 * except fl_st, which is written between fl_lock() and fl_unlock().
 */
#pragma once

#include "aos_flasher.h"
#include "aos_io.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* The job's own copy of what it works with */
typedef struct {
    aos_flasher_job_t job;
    char port[AOS_IO_PORT_NAME_MAX];
    aos_flasher_source_t src;
    aos_flasher_opts_t opts;
    uint64_t t0;
    uint64_t io_ms;             /* time spent moving data, for the speed */
    uint32_t io_bytes;
} fl_job_t;

/* "J" in these files; the symbol has a name that cannot clash */
extern fl_job_t fl_job;
#define J fl_job
extern aos_flasher_status_t fl_st;
extern volatile bool fl_cancel;

void fl_lock(void);
void fl_unlock(void);

enum { L_INFO = 0, L_OK, L_WARN, L_ERR };
void fl_log_v(int level, const char *fmt, va_list ap);
void fl_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Sets the status' error and logs it; returns false, for "return fl_fail(...)". */
bool fl_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fl_phase(aos_flasher_phase_t ph);
/* 'add' bytes more of the job's total done; the speed and the time left come
 * from J.io_ms and J.io_bytes, which the family keeps. */
void fl_progress(uint32_t add);

/* The dump: a file in aos_flasher_dump_dir() named after the chip in fl_st
 * (so read_info first) and the date. open sets the job's total; write adds
 * to the SHA-256 and the progress; close keeps it (and writes its .json,
 * with the family's own fields in 'extra', "\"key\":value,..." or NULL)
 * when ok, and removes it otherwise. */
bool fl_dump_open(uint32_t from, uint32_t len);
bool fl_dump_write(const void *buf, uint32_t n);
void fl_dump_close(bool ok, bool verified, const char *extra);

/* The families: each runs a whole job (J) on J.port. */
bool fl_esp_run(void);
bool fl_avr_run(void);
bool fl_swd_run(void);
bool fl_stm32_run(void);
/* Not a family: the port over TCP (aos_flasher_bridge.c), and the board as
 * the computer's SWD probe (aos_flasher_dap.c) */
bool fl_bridge_run(void);
bool fl_probe_run(void);
/* aos_flasher_swd.c: J.port's pins as an SWD wire (TX SWCLK, RX SWDIO, EN
 * NRST), for the SWD family and the probe; NULL after fl_fail */
aos_io_swd_t *fl_swd_open(uint32_t khz);

/* The console's queue to the target (aos_flasher_console_send): up to max
 * bytes out of it. */
int fl_console_take(uint8_t *out, int max);
