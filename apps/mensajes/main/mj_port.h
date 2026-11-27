/*
 * P4OS - Mensajes: the few things the protocol code takes from where it runs.
 *
 * The protocol (mj_tl.c, mj_crypto.c, mj_mtp.c) is plain C that also builds
 * on the computer, for the tests in tests/ (MJ_HOST): there the log is
 * stderr, the random bytes come from the system and SHA-256 is a copy of
 * its own. In the app they are the HAL's.
 *
 * mj_malloc and friends count what they hand out, which is how the app
 * measures what it holds (mj_mem_used(), in the log and on the settings
 * page): every buffer of the protocol, the arenas and the store go through
 * here. From 1 KB up malloc lands in PSRAM on the board by itself
 * (docs/MEMORY.md); the small ones are asked of PSRAM explicitly, so that
 * thousands of messages do not drain the internal RAM.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void   mj_port_init(void);    /* once, before any thread                */
void  *mj_malloc(size_t n);
void  *mj_calloc(size_t n);
void  *mj_realloc(void *p, size_t n);
void   mj_free(void *p);
char  *mj_strdup(const char *s);
size_t mj_mem_used(void);         /* bytes held now                         */
size_t mj_mem_peak(void);         /* the most held at once                  */

void mj_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mj_random(void *buf, size_t n);
uint32_t mj_now_ms(void);           /* a monotonic clock                      */
int64_t  mj_unixtime(void);         /* the wall clock, 0 when it is not set   */

void mj_sha256(const void *d, size_t n, uint8_t out[32]);
/* SHA-256 of two pieces one after the other, which is what MTProto hashes. */
void mj_sha256_2(const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]);

/* TCP, the HAL's on the board (aos_hal_tcp_*, four slots for the whole
 * system) and plain sockets in the tests. connect: a handle > 0 or < 0;
 * recv: bytes, 0 nothing within the time, < 0 closed or broken; send: all
 * of it or < 0. */
int  mj_tcp_connect(const char *host, int port, int timeout_ms);
int  mj_tcp_send(int h, const void *d, int n, int timeout_ms);
int  mj_tcp_recv(int h, void *buf, int max, int timeout_ms);
void mj_tcp_close(int h);

/* Locks, a thread, and the preferences (aos_hal_pref_* on the board; a
 * text file named by $MJ_PREFS in the tests). */
void *mj_mutex_new(void);
void  mj_mutex_lock(void *m);
void  mj_mutex_unlock(void *m);
bool  mj_thread_start(const char *name, void (*fn)(void *), void *arg, uint32_t stack);
void  mj_sleep_ms(uint32_t ms);

bool mj_pref_get_str(const char *key, char *out, size_t n);
bool mj_pref_set_str(const char *key, const char *val);
bool mj_pref_get_i32(const char *key, int32_t *out);
bool mj_pref_set_i32(const char *key, int32_t v);
void mj_pref_erase(const char *key);
