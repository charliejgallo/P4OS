/* P4OS - Vision: esp-dl's logging in the simulator, to stderr (errors and warnings only). */
#pragma once
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline uint32_t esp_log_timestamp(void) { return 0; }
#ifdef __cplusplus
}
#endif
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) do { if (getenv("VE_VERBOSE")) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#include <stdlib.h>
