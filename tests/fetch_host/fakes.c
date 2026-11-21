/* What aos_fetch.c, aos_tls.c and aos_crypto.c ask of the HAL, on the Mac. */
#include "aos_hal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { void (*fn)(void *); void *arg; } th_t;

static void *th_main(void *p)
{
    th_t t = *(th_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

bool aos_hal_thread_start(const char *name, void (*fn)(void *arg), void *arg, uint32_t stack_bytes, int prio)
{
    (void)name; (void)stack_bytes; (void)prio;
    th_t *t = malloc(sizeof *t);
    t->fn = fn;
    t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, th_main, t)) { free(t); return false; }
    pthread_detach(th);
    return true;
}

void aos_hal_sleep_ms(uint32_t ms) { usleep(ms * 1000); }

void *aos_hal_mutex_create(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    return m;
}
void aos_hal_mutex_lock(void *m)   { pthread_mutex_lock(m); }
void aos_hal_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

uint64_t aos_hal_uptime_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* FAKE_NO_CLOCK=1: the board before the network gave it the time */
bool aos_hal_time_is_valid(void) { return !getenv("FAKE_NO_CLOCK"); }

void *aos_hal_io_alloc(size_t bytes) { return malloc(bytes); }
void  aos_hal_io_free(void *p) { free(p); }
const char *aos_hal_firmware_version(void) { return "test"; }

void aos_hal_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
