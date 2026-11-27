/*
 * P4OS - Vision, the engine on the board: what esp-dl needs from the C++
 * runtime, ESP-IDF and libgcc that the firmware does not export to apps.
 *
 * esp-dl is built with -fno-exceptions and -fno-rtti, std::string, std::map
 * and std::list compiled in from the headers (C++20); what the headers leave
 * to libstdc++ is here or in rt/ (tree.cc and list.cc, GCC's own). All of it
 * ends up local to the module: board.cmake leaves ve_module_api the only
 * global, so nothing here can clash with the firmware or another module.
 *
 *   - operator new and delete take PSRAM (the firmware's malloc keeps blocks
 *     under 1 KB in internal RAM, and esp-dl makes many small objects);
 *   - libstdc++'s error hooks can only mean a bug: they log and stop;
 *   - ESP-IDF's logging goes to the system's log (errors and warnings);
 *   - one core: the dual-core runtime of dl_module_base.cpp is left out
 *     (VE_SINGLE_CORE), so FreeRTOS is not needed;
 *   - pthread mutexes over the HAL's, for the module creator's registry.
 */
#ifndef AOS_SIM

#include "aos_hal.h"

#include <math.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <typeinfo>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_mmu_map.h"

/* ---- memory ------------------------------------------------------------- */

static void *ve_rt_alloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

[[noreturn]] static void ve_rt_fatal(const char *what)
{
    aos_hal_log("vision", "engine runtime: %s", what ? what : "?");
    __builtin_trap();
}

void *operator new(size_t n)
{
    void *p = ve_rt_alloc(n);
    if (!p) ve_rt_fatal("out of memory");
    return p;
}
void *operator new[](size_t n)
{
    return operator new(n);
}
void *operator new(size_t n, const std::nothrow_t &) noexcept
{
    return ve_rt_alloc(n);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept
{
    return ve_rt_alloc(n);
}
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

namespace std {
const nothrow_t nothrow{};

void __throw_bad_alloc(void) { ve_rt_fatal("bad_alloc"); }
void __throw_bad_array_new_length(void) { ve_rt_fatal("bad_array_new_length"); }
void __throw_length_error(const char *s) { ve_rt_fatal(s); }
void __throw_logic_error(const char *s) { ve_rt_fatal(s); }
void __throw_out_of_range(const char *s) { ve_rt_fatal(s); }
void __throw_out_of_range_fmt(const char *s, ...) { ve_rt_fatal(s); }
void __throw_bad_function_call(void) { ve_rt_fatal("bad_function_call"); }

/* make_shared asks whether it is looking at its own tag: never, without RTTI */
bool _Sp_make_shared_tag::_S_eq(const type_info &) noexcept
{
    return false;
}
} // namespace std

/* libstdc++ keeps this one member extern even in C++20 (basic_string.tcc). */
template void std::basic_string<char>::_M_replace_cold(char *, size_type, const char *, size_type, size_type);

extern "C" {

void __cxa_pure_virtual(void)
{
    ve_rt_fatal("pure virtual call");
}

int atexit(void (*fn)(void))
{
    (void)fn;  /* the module's statics die with it (aos_hal_module_close) */
    return 0;
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    aos_hal_log("vision", "assert %s at %s:%d (%s)", expr ? expr : "?", file ? file : "?", line, func ? func : "?");
    __builtin_trap();
}

void _esp_error_check_failed(esp_err_t rc, const char *file, int line, const char *function, const char *expression)
{
    aos_hal_log("vision", "ESP_ERROR_CHECK %d: %s at %s:%d", rc, expression ? expression : "?", file ? file : "?", line);
    __builtin_trap();
}

/* ---- libgcc and libm ------------------------------------------------------ */

int __clzsi2(unsigned int x)
{
    int n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

int __ctzsi2(unsigned int x)
{
    int n = 0;
    if (!x) return 32;
    while (!(x & 1u)) {
        x >>= 1;
        n++;
    }
    return n;
}

/* only for sizes (dl_tool.cpp): whole numbers well inside a float */
double floor(double x)
{
    return (double)floorf((float)x);
}

float tanhf(float x)
{
    if (x > 9.f) return 1.f;
    if (x < -9.f) return -1.f;
    float e = expf(2.f * x);
    return (e - 1.f) / (e + 1.f);
}

/* to nearest, ties to even, as the default rounding mode does */
long lrintf(float x)
{
    float r = roundf(x);
    if (fabsf(r - x) == 0.5f) r = 2.f * roundf(x * 0.5f);
    return (long)r;
}

/* ---- ESP-IDF ---------------------------------------------------------------- */

void esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{
    if (config.opts.log_level > ESP_LOG_WARN) return;
    char line[200];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    aos_hal_log("vision", "%s: %s", tag ? tag : "esp-dl", line);
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    if (level > ESP_LOG_WARN) return;
    char line[200];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    aos_hal_log("vision", "%s: %s", tag ? tag : "esp-dl", line);
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

int printf(const char *format, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    aos_hal_log("vision", "%s", line);
    return n;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)aos_hal_uptime_us();
}

void esp_fill_random(void *buf, size_t len)
{
    aos_hal_random(buf, len);
}

void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(alignment, n * size, caps);
    if (p) memset(p, 0, n * size);
    return p;
}

/* esp-dl sizes its plans and its messages with these; the PSRAM is what
 * counts, and the models of the packs fit in it many times over */
size_t heap_caps_get_free_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? 0 : (16u << 20);
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? 0 : (8u << 20);
}
size_t heap_caps_get_total_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? 0 : (32u << 20);
}

/* where a model's bytes are: always the app's PSRAM buffers here */
esp_err_t esp_mmu_vaddr_to_paddr(void *vaddr, esp_paddr_t *out_paddr, mmu_target_t *out_target)
{
    if (out_paddr) *out_paddr = (esp_paddr_t)(uintptr_t)vaddr;
    if (out_target) *out_target = esp_ptr_external_ram(vaddr) ? MMU_TARGET_PSRAM0 : MMU_TARGET_FLASH0;
    return ESP_OK;
}

/* esp-dl's attention waits for its other core with taskYIELD(): one core here */
void vPortYield(void)
{
    aos_hal_sleep_ms(0);
}

/* ---- pthread mutexes over the HAL's ------------------------------------------ */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    (void)attr;
    void *h = aos_hal_mutex_create();
    if (!h) return -1;
    *m = (pthread_mutex_t)(uintptr_t)h;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    /* the HAL has no delete: a mutex or two for the module's life */
    *m = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (*m == 0 || *m == (pthread_mutex_t)_PTHREAD_MUTEX_INITIALIZER) pthread_mutex_init(m, nullptr);
    aos_hal_mutex_lock((void *)(uintptr_t)*m);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    aos_hal_mutex_unlock((void *)(uintptr_t)*m);
    return 0;
}

} // extern "C"

#endif /* AOS_SIM */
