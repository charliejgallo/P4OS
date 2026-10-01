#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bench.h"

#define RING_BYTES (64 * 1024)

static char *s_ring;
static size_t s_head;       /* next write position */
static bool s_wrapped;
static SemaphoreHandle_t s_mutex;

void bench_log_init(void)
{
    s_ring = heap_caps_calloc(1, RING_BYTES, MALLOC_CAP_SPIRAM);
    s_mutex = xSemaphoreCreateMutex();
}

int64_t bench_us(void) { return esp_timer_get_time(); }

static void ring_put(const char *s)
{
    if (!s_ring) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (const char *p = s; *p; p++) {
        s_ring[s_head++] = *p;
        if (s_head == RING_BYTES) { s_head = 0; s_wrapped = true; }
    }
    xSemaphoreGive(s_mutex);
}

size_t bench_log_dump(char *buf, size_t cap)
{
    if (!s_ring || cap == 0) return 0;
    size_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_wrapped) {
        /* skip the partial first line after the wrap point */
        size_t i = s_head;
        while (i < RING_BYTES && s_ring[i] != '\n') i++;
        for (i++; i < RING_BYTES && n < cap - 1; i++) buf[n++] = s_ring[i];
    }
    for (size_t i = 0; i < s_head && n < cap - 1; i++) buf[n++] = s_ring[i];
    xSemaphoreGive(s_mutex);
    buf[n] = 0;
    return n;
}

void bench_report(const char *test, const char *fmt, ...)
{
    char line[384];
    int n = snprintf(line, sizeof line, "BENCH %s ", test);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
    strlcat(line, "\n", sizeof line);
    ring_put(line);
    line[strlen(line) - 1] = 0;
    lvport_ui_log(line + 6);
}

void bench_say(const char *fmt, ...)
{
    char line[256] = ">> ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + 3, sizeof line - 3, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
    lvport_ui_log(line);
}

static nvs_handle_t cfg_nvs(void)
{
    static nvs_handle_t h;
    if (!h) nvs_open("p4bench", NVS_READWRITE, &h);
    return h;
}

int32_t bench_cfg_get(const char *key, int32_t def)
{
    int32_t v;
    return nvs_get_i32(cfg_nvs(), key, &v) == ESP_OK ? v : def;
}

void bench_cfg_set(const char *key, int32_t val)
{
    nvs_set_i32(cfg_nvs(), key, val);
    nvs_commit(cfg_nvs());
}

bool bench_cfg_get_str(const char *key, char *out, size_t cap)
{
    size_t len = cap;
    return nvs_get_str(cfg_nvs(), key, out, &len) == ESP_OK;
}

void bench_cfg_set_str(const char *key, const char *val)
{
    nvs_set_str(cfg_nvs(), key, val);
    nvs_commit(cfg_nvs());
}

int arg_int(int argc, char **argv, int i, int def)
{
    return i < argc ? (int)strtol(argv[i], NULL, 0) : def;
}

const char *arg_str(int argc, char **argv, int i, const char *def)
{
    return i < argc ? argv[i] : def;
}
