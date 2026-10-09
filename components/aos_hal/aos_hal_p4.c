/*
 * P4OS - HAL for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (phase 3, first
 * block).
 *
 * What is here: the panel (MIPI-DSI, HX8394, two frame buffers in PSRAM),
 * LVGL with its own task and lock, the logical rotation done in the flush
 * by the PPA, the GT911, preferences in NVS, the microSD, the time, the log
 * and the basic system facts. Everything else in aos_hal.h still answers
 * "not available" through aos_hal_stubs.c (weak), and gets written here a
 * block at a time: Wi-Fi through the C6, audio, the header, USB.
 *
 * The display path is the one p4bench measured on the board on 2026-09-28
 * (bench/results/20260928-resumen.md): LVGL renders partial buffers in
 * LOGICAL coordinates and the flush hands each one to esp_lcd's DMA2D copy
 * into the single frame buffer, asynchronously; LVGL gets the buffer back
 * from the copy's done callback, and meanwhile renders the next one into
 * the other buffer. Turned, each area is first rotated by the CPU inside
 * internal RAM, where the scattered writes are cheap. 35 fps upright and 22
 * turned on LVGL's benchmark, no visible tearing. The PPA writing PSRAM was
 * 11 times slower than DMA2D on this chip (rev 1.3), so it is not used here.
 */
#include "aos_hal.h"
#include "aos_notif_internal.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_cache.h"
#include "esp_app_desc.h"
#include "esp_ldo_regulator.h"
#include "esp_async_memcpy.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_hx8394.h"
#include "esp_lcd_touch.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include <dirent.h>
#include <sys/stat.h>
#include "driver/sdmmc_host.h"
#include "driver/ppa.h"
#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "lvgl.h"
#include "cJSON.h"

static const char *TAG = "hal";

#define FB_BYTES (AOS_PANEL_W * AOS_PANEL_H * 2)
/* Each draw buffer: 80 rows of the upright width (45 of the turned one), in
 * internal RAM. LVGL would like a tenth of the screen, but internal RAM is
 * the scarce thing and the flush measured the same at 80 and 128 rows. */
#define DRAW_BYTES (AOS_PANEL_W * 80 * 2)

/* -------------------------------------------------------------------------- */
/* Log, time, system                                                           */
/* -------------------------------------------------------------------------- */

void aos_hal_log(const char *tag, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ESP_LOGI(tag ? tag : "aos", "%s", buf);     /* reaches the ring through the hook below */
}

/* Every ESP_LOG line also goes to the ring, for the portal's log page. The
 * colour codes are dropped there; the console keeps them. */
static vprintf_like_t s_log_prev;

static int log_hook(const char *fmt, va_list ap)
{
    char buf[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t l = n < (int)sizeof buf ? (size_t)n : sizeof buf - 1;
        char clean[256];
        size_t k = 0;
        for (size_t i = 0; i < l; i++) {
            if (buf[i] == 0x1B) { while (i < l && buf[i] != 'm') i++; continue; }
            clean[k++] = buf[i];
        }
        aos_logring_add(clean, k);
    }
    return s_log_prev ? s_log_prev(fmt, ap) : vprintf(fmt, ap);
}

void aos_hal_reboot(void) { esp_restart(); }

uint64_t aos_hal_uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void aos_hal_heap_info(uint32_t *free_internal, uint32_t *free_psram)
{
    if (free_internal) *free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_psram) *free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static char s_app_ctx[48];
void aos_hal_app_context_set(const char *app_id) { snprintf(s_app_ctx, sizeof s_app_ctx, "%s", app_id ? app_id : ""); }
const char *aos_hal_app_context(void) { return s_app_ctx[0] ? s_app_ctx : NULL; }

const char *aos_hal_board_name(void) { return "Waveshare ESP32-P4-WIFI6-Touch-LCD-5"; }
const char *aos_hal_firmware_version(void) { return esp_app_get_description()->version; }

uint32_t aos_hal_caps(void)
{
    /* What the board has and this firmware already drives. Grows with each
     * block: Wi-Fi, BLE, audio and the header join as they are written. */
    extern bool aos_net_p4_up(void);
    extern bool aos_audio_p4_up(void);
    return AOS_CAP_MULTITOUCH | AOS_CAP_ROTATION | AOS_CAP_HEADER | (aos_net_p4_up() ? AOS_CAP_WIFI : 0) |
           (aos_audio_p4_up() ? AOS_CAP_MIC | AOS_CAP_SPEAKER | AOS_CAP_DUPLEX : 0);
}

static char s_tz[64] = "<-03>3";    /* Buenos Aires until Settings says otherwise */

void aos_hal_time_now(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
}

bool aos_hal_time_set(const struct tm *t)
{
    struct tm c = *t;
    time_t v = mktime(&c);
    struct timeval tv = { .tv_sec = v };
    return settimeofday(&tv, NULL) == 0;
}

bool aos_hal_time_is_valid(void) { return time(NULL) > 1700000000; }

void aos_hal_timezone_set(const char *tz)
{
    snprintf(s_tz, sizeof s_tz, "%s", tz ? tz : "UTC0");
    setenv("TZ", s_tz, 1);
    tzset();
    aos_hal_pref_set_str("tz", s_tz);
}

const char *aos_hal_timezone_get(void) { return s_tz; }

/* -------------------------------------------------------------------------- */
/* Preferences (NVS namespace "p4os")                                          */
/* -------------------------------------------------------------------------- */

static nvs_handle_t s_nvs;

/* Every NVS call goes through aos_flash_call (aos_flashop_p4.c): a thread
 * whose stack is in PSRAM may not touch the flash itself. */
void aos_flash_call(void (*fn)(void *ctx), void *ctx);

enum { PREF_GET_I32, PREF_SET_I32, PREF_GET_STR, PREF_SET_STR, PREF_ERASE, PREF_LIST };

typedef struct { char key[NVS_KEY_NAME_MAX_SIZE]; bool str; } pref_key_t;

typedef struct {
    int op;
    const char *key;
    int32_t i32, *i32_out;
    const char *str;
    char *str_out;
    size_t str_len;
    pref_key_t *keys;           /* PREF_LIST */
    int nkeys, cap;
    bool ok;
} pref_op_t;

static bool nvs_ok(void)
{
    if (s_nvs) return true;
    return nvs_open("p4os", NVS_READWRITE, &s_nvs) == ESP_OK;
}

static void pref_do(void *arg)
{
    pref_op_t *o = arg;
    o->ok = false;
    if (!nvs_ok()) return;
    switch (o->op) {
    case PREF_GET_I32:
        o->ok = nvs_get_i32(s_nvs, o->key, o->i32_out) == ESP_OK;
        break;
    case PREF_SET_I32:
        o->ok = nvs_set_i32(s_nvs, o->key, o->i32) == ESP_OK && nvs_commit(s_nvs) == ESP_OK;
        break;
    case PREF_GET_STR: {
        size_t len = o->str_len;
        o->ok = nvs_get_str(s_nvs, o->key, o->str_out, &len) == ESP_OK;
        break;
    }
    case PREF_SET_STR:
        o->ok = nvs_set_str(s_nvs, o->key, o->str) == ESP_OK && nvs_commit(s_nvs) == ESP_OK;
        break;
    case PREF_ERASE:
        o->ok = nvs_erase_key(s_nvs, o->key) == ESP_OK;
        nvs_commit(s_nvs);
        break;
    case PREF_LIST: {
        nvs_iterator_t it = NULL;
        esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "p4os", NVS_TYPE_ANY, &it);
        while (e == ESP_OK && o->nkeys < o->cap) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.type == NVS_TYPE_I32 || info.type == NVS_TYPE_STR) {
                pref_key_t *k = &o->keys[o->nkeys++];
                snprintf(k->key, sizeof k->key, "%s", info.key);
                k->str = info.type == NVS_TYPE_STR;
            }
            e = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        o->ok = true;
        break;
    }
    }
}

static bool pref_run(pref_op_t *o)
{
    aos_flash_call(pref_do, o);
    return o->ok;
}

bool aos_hal_pref_get_i32(const char *key, int32_t *out)
{
    return pref_run(&(pref_op_t){ .op = PREF_GET_I32, .key = key, .i32_out = out });
}

bool aos_hal_pref_set_i32(const char *key, int32_t value)
{
    return pref_run(&(pref_op_t){ .op = PREF_SET_I32, .key = key, .i32 = value });
}

bool aos_hal_pref_get_str(const char *key, char *out, size_t out_len)
{
    return pref_run(&(pref_op_t){ .op = PREF_GET_STR, .key = key, .str_out = out, .str_len = out_len });
}

bool aos_hal_pref_set_str(const char *key, const char *value)
{
    return pref_run(&(pref_op_t){ .op = PREF_SET_STR, .key = key, .str = value });
}

bool aos_hal_pref_erase(const char *key)
{
    return pref_run(&(pref_op_t){ .op = PREF_ERASE, .key = key });
}

/* The keys are listed first and visited afterwards, one read each, so that
 * visit() runs on the caller's stack and not on the flash helper's. */
int aos_hal_pref_foreach(aos_hal_pref_visit_t visit, void *ctx)
{
    pref_op_t o = { .op = PREF_LIST, .cap = 256 };
    o.keys = heap_caps_malloc(o.cap * sizeof *o.keys, MALLOC_CAP_SPIRAM);
    if (!o.keys || !pref_run(&o)) {
        free(o.keys);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < o.nkeys; i++) {
        const pref_key_t *k = &o.keys[i];
        if (k->str) {
            char s[128] = "";
            aos_hal_pref_get_str(k->key, s, sizeof s);
            visit(k->key, true, 0, s, ctx);
        } else {
            int32_t v = 0;
            aos_hal_pref_get_i32(k->key, &v);
            visit(k->key, false, v, NULL, ctx);
        }
        n++;
    }
    free(o.keys);
    return n;
}

/* -------------------------------------------------------------------------- */
/* microSD                                                                     */
/* -------------------------------------------------------------------------- */

#define SD_ROOT "/sdcard"
static sdmmc_card_t *s_card;
/* Disk mode (aos_usb_p4.c): while a computer has the card s_card is NULL;
 * when the computer ejects it, the USB side mounts it back at SD_ROOT for a
 * moment, on its own card struct, and says so here. */
static bool s_sd_back;

/* The card's slot, for the usual mount and for disk mode's raw one. */
static bool sd_host_config(sdmmc_host_t *out_host, sdmmc_slot_config_t *out_slot)
{
    static sd_pwr_ctrl_handle_t pwr;
    if (!pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr) != ESP_OK) return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr;
    /* A buffer that isn't DMA-ready (any malloc(): stdio's 8 KB of each FILE,
     * most apps' buffers) goes through a bounce buffer, and by default the
     * driver took a new 8 KB one from internal DMA RAM for every read and
     * write and gave it back after. One of 32 KB in PSRAM, on a 128-byte
     * line as the SDMMC DMA wants PSRAM, kept for good: fewer, bigger
     * transfers and no churn in internal RAM (2026-09-29). */
    static void *bounce;
    if (!bounce) bounce = heap_caps_aligned_alloc(128, 32 * 1024, MALLOC_CAP_SPIRAM);
    if (bounce) {
        host.dma_aligned_buffer = bounce;
        host.unaligned_multi_block_rw_max_chunk_size = 32 * 1024 / 512;
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    *out_host = host;
    *out_slot = slot;
    return true;
}

static void sd_mount(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    if (!sd_host_config(&host, &slot)) return;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 12,
                                            .allocation_unit_size = 64 * 1024 };
    /* A restart by software does not power the card off, and once (after a
     * restart from the portal, 2026-09-29) the board came up with no card
     * until it was unplugged. So a failed mount is tried twice more, after
     * a pause, before the board goes on without the card. */
    esp_err_t e = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        e = esp_vfs_fat_sdmmc_mount(SD_ROOT, &host, &slot, &mc, &s_card);
        if (e == ESP_OK) {
            if (attempt > 1) ESP_LOGW(TAG, "microSD mounted at attempt %d", attempt);
            break;
        }
        s_card = NULL;
        ESP_LOGW(TAG, "microSD mount, attempt %d: %s", attempt, esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "no microSD (%s)", esp_err_to_name(e));
}

bool aos_hal_sd_present(void) { return s_card != NULL || s_sd_back; }

/* Disk mode (aos_hal.h): the card leaves the board for a computer. */
bool aos_hal_sd_release(void)
{
    if (!s_card) return false;
    /* the player reads the card: an open file under an unmount does not end well */
    aos_hal_player_stop();
    /* unmounting also shuts the SDMMC host and frees the card: disk mode
     * starts from a cold host (aos_p4_sd_card_open) */
    esp_err_t e = esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "microSD unmount for disk mode: %s", esp_err_to_name(e));
        return false;
    }
    s_card = NULL;
    ESP_LOGI(TAG, "microSD released");
    return true;
}

bool aos_hal_sd_reclaim(void)
{
    if (s_card) return true;
    s_sd_back = false;
    sd_mount();
    ESP_LOGI(TAG, "microSD %s", s_card ? "mounted again" : "did not come back");
    return s_card != NULL;
}

void aos_hal_sd_mark_mounted(bool mounted) { s_sd_back = mounted; }

/* For aos_usb_p4.c, not in aos_hal.h: the card initialised on the same slot
 * with no filesystem on top, for the USB side to read and write sectors. */
sdmmc_card_t *aos_p4_sd_card_open(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    if (!sd_host_config(&host, &slot)) return NULL;
    sdmmc_card_t *card = calloc(1, sizeof *card);
    if (!card) return NULL;
    esp_err_t e = host.init();
    if (e == ESP_OK) e = sdmmc_host_init_slot(host.slot, &slot);
    if (e == ESP_OK) e = sdmmc_card_init(&host, card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "microSD init for disk mode: %s", esp_err_to_name(e));
        sdmmc_host_deinit_slot(host.slot);   /* the last slot shuts the host too */
        free(card);
        return NULL;
    }
    return card;
}

void aos_p4_sd_card_close(sdmmc_card_t *card)
{
    if (!card) return;
    sdmmc_host_deinit_slot(card->host.slot);
    free(card);
}

void *aos_hal_io_alloc(size_t bytes)
{
    return heap_caps_aligned_alloc(128, (bytes + 127) & ~(size_t)127, MALLOC_CAP_SPIRAM);
}

void aos_hal_io_free(void *p) { heap_caps_free(p); }

bool aos_hal_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!aos_hal_sd_present()) return false;
    return esp_vfs_fat_info(SD_ROOT, total_bytes, free_bytes) == ESP_OK;
}

/* A folder in one pass (aos_hal.h): FatFs's directory records carry the
 * size and the date, so under the card nothing is stat()ed. */
static int dir_scan_posix(const char *path, aos_dir_cb_t cb, void *ctx)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    char full[320];
    while ((e = readdir(d)) != NULL) {
        aos_dir_entry_t de = { .name = e->d_name };
        struct stat st;
        snprintf(full, sizeof full, "%.200s/%.110s", path, e->d_name);
        if (!stat(full, &st)) {
            de.dir = S_ISDIR(st.st_mode);
            de.size = de.dir ? 0 : (uint32_t)st.st_size;
            de.mtime = st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0;
        }
        n++;
        if (!cb(&de, ctx)) break;
    }
    closedir(d);
    return n;
}

int aos_hal_dir_scan(const char *path, aos_dir_cb_t cb, void *ctx)
{
    size_t rl = strlen(SD_ROOT);
    if (!path || !cb) return -1;
    if (!s_card || strncmp(path, SD_ROOT, rl) || (path[rl] && path[rl] != '/')) return dir_scan_posix(path, cb, ctx);
    char fpath[300];
    snprintf(fpath, sizeof fpath, "%c:%.290s", (char)('0' + ff_diskio_get_pdrv_card(s_card)), path[rl] ? path + rl : "/");
    FF_DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, fpath) != FR_OK) return -1;
    int n = 0;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        aos_dir_entry_t de = { .name = fi.fname, .dir = (fi.fattrib & AM_DIR) != 0 };
        de.size = de.dir ? 0 : (uint32_t)fi.fsize;
        if (fi.fdate) {
            struct tm t = {
                .tm_year = ((fi.fdate >> 9) & 0x7F) + 80, .tm_mon = ((fi.fdate >> 5) & 0xF) - 1, .tm_mday = fi.fdate & 0x1F,
                .tm_hour = (fi.ftime >> 11) & 0x1F, .tm_min = (fi.ftime >> 5) & 0x3F, .tm_sec = (fi.ftime & 0x1F) * 2,
                .tm_isdst = -1,
            };
            time_t tt = mktime(&t);         /* FAT keeps local time, as the VFS assumes */
            de.mtime = tt > 0 ? (uint32_t)tt : 0;
        }
        n++;
        if (!cb(&de, ctx)) break;
    }
    f_closedir(&dir);
    return n;
}

/* P4OS Monitor: the card beyond its usage (aos_hal.h, aos_sd_info_t). */
bool aos_hal_sd_info(aos_sd_info_t *out)
{
    if (!out || !s_card) return false;
    memset(out, 0, sizeof *out);
    const sdmmc_card_t *c = s_card;
    snprintf(out->name, sizeof out->name, "%.7s", c->cid.name);
    out->capacity = (uint64_t)c->csd.capacity * c->csd.sector_size;
    out->kind = c->is_mmc ? "MMC" : !(c->ocr & SD_OCR_SDHC_CAP) ? "SDSC"
              : out->capacity > 32ULL * 1024 * 1024 * 1024 ? "SDXC" : "SDHC";
    out->freq_khz = (uint32_t)c->real_freq_khz;
    out->bus_width = 1 << c->log_bus_width;
    out->manufacturer = c->cid.mfg_id;
    if (!c->is_mmc && c->cid.date) {
        out->year = 2000 + ((c->cid.date >> 4) & 0xFF);
        out->month = c->cid.date & 0xF;
    }
    out->fs = "";
    char drv[4] = { (char)('0' + ff_diskio_get_pdrv_card(c)), ':', 0 };
    FATFS *fs = NULL;
    DWORD nfree;
    if (f_getfree(drv, &nfree, &fs) == FR_OK && fs) {
        out->fs = fs->fs_type == FS_FAT12 ? "FAT12" : fs->fs_type == FS_FAT16 ? "FAT16"
                : fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_EXFAT ? "exFAT" : "";
    }
    return true;
}

const char *aos_hal_path_sd_root(void)   { return aos_hal_sd_present() ? SD_ROOT : NULL; }
const char *aos_hal_path_apps(void)      { return SD_ROOT "/apps"; }
const char *aos_hal_path_photos(void)    { return SD_ROOT "/photos"; }
const char *aos_hal_path_music(void)     { return SD_ROOT "/music"; }
const char *aos_hal_path_recordings(void){ return SD_ROOT "/recordings"; }
const char *aos_hal_path_data(void)      { return SD_ROOT "/data"; }
const char *aos_hal_path_scans(void)     { return SD_ROOT "/redes"; }
const char *aos_hal_path_lang(void)      { return SD_ROOT "/lang"; }
const char *aos_hal_path_icons(void)     { return SD_ROOT "/icons"; }
const char *aos_hal_path_menu(void)      { return SD_ROOT "/menu.txt"; }

/* -------------------------------------------------------------------------- */
/* Panel                                                                       */
/* -------------------------------------------------------------------------- */

static esp_lcd_panel_handle_t s_panel;
/* The DPI's frame buffers, in PSRAM: three, so an app can draw a whole
 * frame into one the panel does not show and flip to it
 * (aos_hal_display_back/flip) while another waits for the next refresh.
 * s_fb is the one LVGL, the blits and the capture use: the last one flipped
 * to, which the panel shows from its next refresh on. Pref "fbs" = 1 goes
 * back to a single buffer. */
static uint16_t *s_fbs[3];
static int s_nfbs = 1;
static volatile int s_req;              /* the buffer asked for last (s_fb) */
static volatile int s_scan;             /* the one the DPI scans since its last refresh */
static uint32_t s_flip_seq;             /* flips so far */
static uint32_t s_buf_flip[3];          /* the flip that last showed each buffer, 0 never */
static volatile bool s_buf_lvgl[3];     /* LVGL flushed into it since that flip */
static uint16_t *s_fb;
static SemaphoreHandle_t s_frame_sem;
static int s_brightness = 80;

static bool IRAM_ATTR on_frame(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    /* the DMA has just started the next refresh from the buffer asked for */
    s_scan = s_req;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

/* esp_lcd's DMA2D copy of an area ended: LVGL may reuse its buffer. Only
 * for a copy the flush started (s_copy_pending). */
static lv_display_t *s_disp;
static volatile bool s_copy_pending;

static volatile bool s_blit_copy;       /* the DMA2D copy in flight is a blit's, not LVGL's */

static volatile bool s_piece_wait;      /* a turned area's piece, not its last: the flush waits on it */
static SemaphoreHandle_t s_piece_sem;

static bool IRAM_ATTR on_copy_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    if (s_blit_copy) {
        s_blit_copy = false;
        return false;
    }
    if (s_piece_wait) {
        s_piece_wait = false;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_piece_sem, &woken);
        return woken == pdTRUE;
    }
    if (s_copy_pending && s_disp) {
        s_copy_pending = false;
        lv_display_flush_ready(s_disp);
    }
    return false;
}

static void cache_flush(const void *p, size_t len)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)127;
    uintptr_t b = ((uintptr_t)p + len + 127) & ~(uintptr_t)127;
    esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t panel_init(void)
{
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    static esp_ldo_channel_handle_t phy_ldo;
    esp_ldo_channel_config_t ldo = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &phy_ldo));

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = { .bus_id = 0, .num_data_lanes = 2, .phy_clk_src = 0, .lane_bit_rate_mbps = 700 };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi, &io));

    esp_lcd_dpi_panel_config_t dpi = HX8394_720_1280_PANEL_30HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    int32_t nfbs = 3;
    aos_hal_pref_get_i32("fbs", &nfbs);
    s_nfbs = nfbs == 1 ? 1 : 3;
    dpi.num_fbs = s_nfbs;
    dpi.flags.use_dma2d = true;
    hx8394_vendor_config_t vendor = { .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi, .lane_num = 2 } };
    esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 16, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BSP_LCD_RST, .vendor_config = &vendor, .flags.reset_active_high = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_hx8394(io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    void *a = NULL, *b = NULL, *c = NULL;
    if (s_nfbs == 3) ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, &a, &b, &c));
    else ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &a));
    s_fbs[0] = a;
    s_fbs[1] = b;
    s_fbs[2] = c;
    s_fb = a;
    s_frame_sem = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame, .on_color_trans_done = on_copy_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);
    memset(s_fb, 0, FB_BYTES);
    cache_flush(s_fb, FB_BYTES);
    esp_lcd_panel_disp_on_off(s_panel, true);
    int32_t bl = 80;
    aos_hal_pref_get_i32("bright", &bl);
    aos_hal_brightness_set(bl);
    return ESP_OK;
}

int aos_hal_brightness_get(void) { return s_brightness; }

void aos_hal_brightness_set(int percent)
{
    if (percent < 1) percent = 1;          /* 0 is "off": aos_hal_display_on() */
    if (percent > 100) percent = 100;
    s_brightness = percent;
    bsp_display_brightness_set(percent);
    aos_hal_pref_set_i32("bright", percent);
}

/* The screen's auto-off. Pref "scr_off", in seconds: 0 (the default) never
 * switches it off, since this is a bench device plugged in. With a timeout
 * the screen dims to a third ten seconds before going dark, like a phone's;
 * any activity brings it back. aos_hal_screen_idle_tick() runs the policy
 * from LVGL's thread (aos_ui_tick). */
static bool s_display_on = true;
static bool s_dimmed;
static uint32_t s_off_s;
static bool s_off_loaded;
static volatile int64_t s_last_act_us;
#define DIM_BEFORE_S 10

void aos_hal_display_on(bool on)
{
    s_display_on = on;
    s_dimmed = false;
    if (on) s_last_act_us = esp_timer_get_time();
    bsp_display_brightness_set(on ? s_brightness : 0);
}
bool aos_hal_display_is_on(void) { return s_display_on; }

void aos_hal_activity(void)
{
    s_last_act_us = esp_timer_get_time();
    if (!s_display_on) aos_hal_display_on(true);
    else if (s_dimmed) {
        s_dimmed = false;
        bsp_display_brightness_set(s_brightness);
    }
}

static void off_load(void)
{
    if (s_off_loaded) return;
    s_off_loaded = true;
    int32_t v = 0;
    aos_hal_pref_get_i32("scr_off", &v);
    s_off_s = v > 0 ? (uint32_t)v : 0;
}

void aos_hal_screen_timeouts_set(uint32_t active_s, uint32_t aod_s)
{
    (void)aod_s;                        /* no always-on display here */
    off_load();
    s_off_s = active_s;
    aos_hal_pref_set_i32("scr_off", (int32_t)active_s);
    aos_hal_activity();
}

void aos_hal_screen_timeouts_get(uint32_t *active_s, uint32_t *aod_s)
{
    off_load();
    if (active_s) *active_s = s_off_s;
    if (aod_s) *aod_s = 0;
}

void aos_hal_screen_idle_tick(bool keep_awake)
{
    off_load();
    if (keep_awake) aos_hal_activity();
    if (!s_off_s || !s_display_on) return;
    int64_t idle_s = (esp_timer_get_time() - s_last_act_us) / 1000000;
    if (idle_s >= (int64_t)s_off_s) {
        ESP_LOGI(TAG, "screen off after %u s idle", (unsigned)idle_s);
        aos_hal_display_on(false);
    } else if (!s_dimmed && s_off_s > 2 * DIM_BEFORE_S && idle_s >= (int64_t)s_off_s - DIM_BEFORE_S) {
        ESP_LOGI(TAG, "screen dimmed, %u s idle", (unsigned)idle_s);
        s_dimmed = true;
        bsp_display_brightness_set(s_brightness / 3 > 1 ? s_brightness / 3 : 1);
    }
}

/* -------------------------------------------------------------------------- */
/* LVGL: task, lock, flush with rotation                                       */
/* -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static int s_rot;                  /* 0 / 90 / 180 / 270 */
/* A turned area goes to the panel in pieces rotated into this, in internal
 * RAM, only while the screen is turned. It held a whole area before (the
 * draw buffer's 80 KB), which left a game lying down no two blocks of
 * 20 KB for its bands; pieces of 24 KB free the rest (2026-09-30). */
#define ROT_TMP_BYTES (24 * 1024)
static uint16_t *s_rot_tmp;
static size_t s_draw_bytes = DRAW_BYTES;
static int s_draw_mode;                 /* pref "lvbuf", as it was at boot */
static int s_draw_bufs;                 /* 1 or 2 */

int aos_hal_screen_w(void) { return (s_rot == 90 || s_rot == 270) ? AOS_PANEL_H : AOS_PANEL_W; }
int aos_hal_screen_h(void) { return (s_rot == 90 || s_rot == 270) ? AOS_PANEL_W : AOS_PANEL_H; }
int aos_hal_display_get_rotation(void) { return s_rot; }

/* Called by aos_ui with the lock held. The draw buffers were sized for the
 * longer side, so both orientations fit in them. */
void aos_hal_display_set_rotation(int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees != 90 && degrees != 180 && degrees != 270) degrees = 0;
    for (int i = 0; s_copy_pending && i < 50; i++) vTaskDelay(1);    /* a copy may still read the old buffer */
    /* turned, an area is rotated in internal RAM before the DMA2D copy */
    if (degrees && !s_rot_tmp && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > ROT_TMP_BYTES + 64 * 1024)
        s_rot_tmp = heap_caps_aligned_alloc(128, ROT_TMP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_piece_sem) s_piece_sem = xSemaphoreCreateBinary();
    if (!degrees && s_rot_tmp) { heap_caps_free(s_rot_tmp); s_rot_tmp = NULL; }
    if (degrees && !s_rot_tmp) ESP_LOGW(TAG, "no internal RAM for the rotation buffer: the CPU copies turned areas (slow)");
    s_rot = degrees;
    if (s_disp) {
        lv_display_set_resolution(s_disp, aos_hal_screen_w(), aos_hal_screen_h());
    }
}

static void rotate_area(const lv_area_t *a, lv_area_t *p)
{
    switch (s_rot) {
    case 90:  p->x1 = AOS_PANEL_W - 1 - a->y2; p->x2 = AOS_PANEL_W - 1 - a->y1; p->y1 = a->x1; p->y2 = a->x2; break;
    case 180: p->x1 = AOS_PANEL_W - 1 - a->x2; p->x2 = AOS_PANEL_W - 1 - a->x1;
              p->y1 = AOS_PANEL_H - 1 - a->y2; p->y2 = AOS_PANEL_H - 1 - a->y1; break;
    case 270: p->x1 = a->y1; p->x2 = a->y2; p->y1 = AOS_PANEL_H - 1 - a->x2; p->y2 = AOS_PANEL_H - 1 - a->x1; break;
    default:  *p = *a;
    }
}

static void copy_cpu(uint16_t *fb, const lv_area_t *a, const uint16_t *src)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    lv_area_t p;
    rotate_area(a, &p);
    for (int y = 0; y < h; y++) {
        const uint16_t *s = src + y * w;
        switch (s_rot) {
        case 0:
            memcpy(fb + (p.y1 + y) * AOS_PANEL_W + p.x1, s, w * 2);
            break;
        case 90: {
            uint16_t *d = fb + a->x1 * AOS_PANEL_W + (AOS_PANEL_W - 1 - (a->y1 + y));
            for (int x = 0; x < w; x++, d += AOS_PANEL_W) *d = s[x];
            break;
        }
        case 180: {
            uint16_t *d = fb + (AOS_PANEL_H - 1 - (a->y1 + y)) * AOS_PANEL_W + (AOS_PANEL_W - 1 - a->x1);
            for (int x = 0; x < w; x++) *d-- = s[x];
            break;
        }
        case 270: {
            uint16_t *d = fb + (AOS_PANEL_H - 1 - a->x1) * AOS_PANEL_W + (a->y1 + y);
            for (int x = 0; x < w; x++, d -= AOS_PANEL_W) *d = s[x];
            break;
        }
        }
    }
    cache_flush(fb + p.y1 * AOS_PANEL_W, (p.y2 - p.y1 + 1) * AOS_PANEL_W * 2);
}

/* Columns cx0 .. cx0 + cw - 1 of an area of rows 'stride' pixels long and
 * h rows, rotated into t, laid out as the panel's rectangle of that piece. */
static void rotate_into(uint16_t *t, const uint16_t *src, int stride, int cx0, int cw, int h)
{
    if (s_rot == 90) {
        for (int y = 0; y < h; y++) { const uint16_t *r = src + y * stride + cx0; for (int x = 0; x < cw; x++) t[x * h + (h - 1 - y)] = r[x]; }
    } else if (s_rot == 180) {
        for (int y = 0; y < h; y++) { const uint16_t *r = src + y * stride + cx0; uint16_t *d = t + (h - 1 - y) * cw + (cw - 1); for (int x = 0; x < cw; x++) *d-- = r[x]; }
    } else {
        for (int y = 0; y < h; y++) { const uint16_t *r = src + y * stride + cx0; for (int x = 0; x < cw; x++) t[(cw - 1 - x) * h + y] = r[x]; }
    }
}

/* A turned area in pieces that fit s_rot_tmp: columns lying down (a column
 * of the screen is a row of the panel), rows upside down. Every piece but
 * the last waits for its DMA2D copy; the last is left running and ends the
 * flush from on_copy_done, as a whole area did. s_copy_pending stays set
 * throughout, so no blit or flip comes in between. false: the DMA2D refused
 * a piece (the caller copies what is left on the CPU; the area comes out
 * whole either way). */
static bool flush_turned(const lv_area_t *area, const uint16_t *src)
{
    const int w = lv_area_get_width(area), h = lv_area_get_height(area);
    const int cap = ROT_TMP_BYTES / 2;
    const bool cols = s_rot == 90 || s_rot == 270;
    int step = cols ? cap / h : cap / w;          /* columns or rows a piece */
    if (step < 1) return false;
    int total = cols ? w : h;
    s_copy_pending = true;
    for (int o = 0; o < total; o += step) {
        int n = o + step > total ? total - o : step;
        lv_area_t sub = *area, p;
        if (cols) { sub.x1 = area->x1 + o; sub.x2 = sub.x1 + n - 1; rotate_into(s_rot_tmp, src, w, o, n, h); }
        else { sub.y1 = area->y1 + o; sub.y2 = sub.y1 + n - 1; rotate_into(s_rot_tmp, src + (size_t)o * w, w, 0, w, n); }
        rotate_area(&sub, &p);
        bool last = o + n >= total;
        s_piece_wait = !last;
        if (esp_lcd_panel_draw_bitmap(s_panel, p.x1, p.y1, p.x2 + 1, p.y2 + 1, s_rot_tmp) != ESP_OK) {
            s_piece_wait = false;
            s_copy_pending = false;
            return false;
        }
        if (last) return true;                    /* on_copy_done ends the flush */
        if (xSemaphoreTake(s_piece_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_piece_wait = false;
            s_copy_pending = false;
            return false;
        }
    }
    return true;
}

static uint64_t s_flush_px;             /* written by the LVGL task only */
static uint32_t s_flush_frames;

bool aos_hal_display_flush_count(uint64_t *px, uint32_t *frames)
{
    if (px) *px = s_flush_px;
    if (frames) *frames = s_flush_frames;
    return true;
}

/* aos_hal.h: the whole screen redrawn `frames` times, as fast as LVGL can,
 * for comparing where it draws (pref "lvbuf"). Takes the LVGL lock: what
 * is on screen is drawn again, nothing changes. */
bool aos_hal_display_bench(int frames, aos_display_bench_t *out)
{
    if (!s_disp || !out || frames < 1) return false;
    if (frames > 200) frames = 200;
    memset(out, 0, sizeof *out);
    if (!aos_hal_lock(2000)) return false;
    uint32_t f0 = s_flush_frames;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(s_disp);
    }
    int64_t t1 = esp_timer_get_time();
    aos_hal_unlock();
    out->frames = frames;
    out->us_per_frame = (uint32_t)((t1 - t0) / frames);
    out->flushes = s_flush_frames - f0;
    out->mode = s_draw_mode;
    out->rows = (int)(s_draw_bytes / (AOS_PANEL_W * 2));
    out->buffers = s_draw_bufs;
    return true;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (s_nfbs == 3) s_buf_lvgl[s_req] = true;     /* it writes into the buffer last flipped to */
    const uint16_t *src = (const uint16_t *)px;
    s_flush_px += (uint64_t)lv_area_get_size(area);
    if (lv_display_flush_is_last(disp)) s_flush_frames++;
    if (s_rot == 0) {
        s_copy_pending = true;
        if (esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, src) == ESP_OK) return;
        s_copy_pending = false;
    } else if (s_rot_tmp && s_piece_sem) {
        if (flush_turned(area, src)) return;
    }
    copy_cpu(s_fb, area, src);         /* no rotation buffer, or the DMA2D refused */
    lv_display_flush_ready(disp);
}

/* -------------------------------------------------------------------------- */
/* Direct blit (aos_hal.h): Video, Doom, the apps that render a frame of their */
/* own and skip LVGL's                                                         */
/* -------------------------------------------------------------------------- */

static ppa_client_handle_t s_blit_ppa;
static int s_blit_hw;                   /* 0 not tried, 1 PPA, -1 CPU only */
static uint32_t s_blit_fail_log;
static void into_mx_take(void);         /* the PPA client, one caller at a time */
static void into_mx_give(void);

static bool blit_ppa_ready(void)
{
    if (s_blit_hw == 0) {
        int32_t on = 1;
        aos_hal_pref_get_i32("blit_hw", &on);
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        s_blit_hw = on && ppa_register_client(&pc, &s_blit_ppa) == ESP_OK ? 1 : -1;
        ESP_LOGI(TAG, "blit: %s", s_blit_hw > 0 ? "PPA" : "CPU");
    }
    return s_blit_hw > 0;
}

/* The screen's (logical) pixel to the panel's, as the flush turns them. */
static inline uint16_t *fb_at(uint16_t *fb, int lx, int ly)
{
    switch (s_rot) {
    case 90:  return fb + lx * AOS_PANEL_W + (AOS_PANEL_W - 1 - ly);
    case 180: return fb + (AOS_PANEL_H - 1 - ly) * AOS_PANEL_W + (AOS_PANEL_W - 1 - lx);
    case 270: return fb + (AOS_PANEL_H - 1 - lx) * AOS_PANEL_W + ly;
    default:  return fb + ly * AOS_PANEL_W + lx;
    }
}

/* Nearest neighbour, turned, in either byte order. Each row starts where
 * fb_at() puts it, and dx is the panel's step for one logical pixel to the
 * right, which makes every orientation the same loop. */
static void blit_cpu(uint16_t *fb, int x, int y, int w, int h, const uint16_t *src, int k, bool be)
{
    const int dx = s_rot == 90 ? AOS_PANEL_W : s_rot == 180 ? -1 : s_rot == 270 ? -AOS_PANEL_W : 1;
    for (int sy = 0; sy < h; sy++) {
        const uint16_t *row = src + (size_t)sy * w;
        for (int ry = 0; ry < k; ry++) {
            uint16_t *d = fb_at(fb, x, y + sy * k + ry);
            if (s_rot == 0 && k == 1 && !be) {
                memcpy(d, row, (size_t)w * 2);
                continue;
            }
            for (int sx = 0; sx < w; sx++) {
                uint16_t v = row[sx];
                if (be) v = (uint16_t)(v << 8 | v >> 8);
                for (int rx = 0; rx < k; rx++, d += dx) *d = v;
            }
        }
    }
}

bool aos_hal_display_blit_scaled(int x, int y, int w, int h, const void *rgb565, int scale, bool big_endian)
{
    if (!s_fb || !rgb565 || w <= 0 || h <= 0 || scale < 1) return false;
    int ow = w * scale, oh = h * scale;
    if (x < 0 || y < 0 || x + ow > aos_hal_screen_w() || y + oh > aos_hal_screen_h()) return false;
    /* LVGL's last flush may still be copying into the framebuffer (it is
     * asynchronous): the blit goes after it, or LVGL's copy lands on top */
    for (int64_t t0 = esp_timer_get_time(); s_copy_pending; ) {
        if (esp_timer_get_time() - t0 > 100000) return false;
        vTaskDelay(1);
    }
    lv_area_t a = { x, y, x + ow - 1, y + oh - 1 }, p;
    rotate_area(&a, &p);
    /* Nothing to scale, turn or swap: the DMA2D copy LVGL's flush uses.
     * The PPA at x1 took 48 ms for a whole 720x1280 frame on the board,
     * the DMA2D 4.4 (2026-09-29). It is asynchronous: wait for it here, so
     * the caller's buffer is free when this returns. */
    if (scale == 1 && s_rot == 0 && !big_endian) {
        s_blit_copy = true;
        if (esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565) == ESP_OK) {
            for (int64_t t0 = esp_timer_get_time(); s_blit_copy; ) {
                if (esp_timer_get_time() - t0 > 200000) { s_blit_copy = false; return false; }
                vTaskDelay(1);
            }
            return true;
        }
        s_blit_copy = false;
    }
    if (blit_ppa_ready()) {
        /* The PPA turns counter-clockwise; the flush turns the picture
         * clockwise for 90 (rotate_area). */
        ppa_srm_rotation_angle_t ang = s_rot == 90  ? PPA_SRM_ROTATION_ANGLE_270 :
                                       s_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                       s_rot == 270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
        ppa_srm_oper_config_t op = {
            .in = {
                .buffer = rgb565, .pic_w = (uint32_t)w, .pic_h = (uint32_t)h,
                .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_fb, .buffer_size = FB_BYTES,
                .pic_w = AOS_PANEL_W, .pic_h = AOS_PANEL_H,
                .block_offset_x = (uint32_t)p.x1, .block_offset_y = (uint32_t)p.y1,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = ang,
            .scale_x = (float)scale,
            .scale_y = (float)scale,
            .byte_swap = big_endian,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        into_mx_take();
        esp_err_t e = ppa_do_scale_rotate_mirror(s_blit_ppa, &op);
        into_mx_give();
        if (e == ESP_OK) return true;
        if (s_blit_fail_log++ < 4) ESP_LOGW(TAG, "blit: the PPA refused (%s), the CPU does it", esp_err_to_name(e));
    }
    blit_cpu(s_fb, x, y, w, h, (const uint16_t *)rgb565, scale, big_endian);
    cache_flush(s_fb + p.y1 * AOS_PANEL_W, (size_t)(p.y2 - p.y1 + 1) * AOS_PANEL_W * 2);
    return true;
}

bool aos_hal_display_blit_native(int x, int y, int w, int h, const void *rgb565)
{
    if (!s_fb || !rgb565 || w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > AOS_PANEL_W || y + h > AOS_PANEL_H) return false;
    for (int64_t t0 = esp_timer_get_time(); s_copy_pending; ) {
        if (esp_timer_get_time() - t0 > 100000) return false;
        vTaskDelay(1);
    }
    s_blit_copy = true;
    if (esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565) == ESP_OK) {
        for (int64_t t0 = esp_timer_get_time(); s_blit_copy; ) {
            if (esp_timer_get_time() - t0 > 200000) { s_blit_copy = false; return false; }
            vTaskDelay(1);
        }
        return true;
    }
    s_blit_copy = false;
    /* the DMA2D refused: the CPU, row by row */
    for (int r = 0; r < h; r++)
        memcpy(s_fb + (size_t)(y + r) * AOS_PANEL_W + x, (const uint16_t *)rgb565 + (size_t)r * w, (size_t)w * 2);
    cache_flush(s_fb + (size_t)y * AOS_PANEL_W, (size_t)h * AOS_PANEL_W * 2);
    return true;
}

/* A rectangle of the screen (x, y, w, h in the screen's own coordinates,
 * turned as the screen is) into a panel buffer that is not shown yet: whole
 * rows upright by the AXI DMA (rows_dma), anything else by the PPA. Two cores may
 * call it at once: the PPA client is taken one at a time. */
static SemaphoreHandle_t s_into_mx;

/* The PPA client is one: whoever calls it (LVGL's thread, a worker, the
 * other core's half) takes it in turn. */
static void into_mx_take(void)
{
    if (!s_into_mx) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&mux);
        if (!s_into_mx) { s_into_mx = m; m = NULL; }
        taskEXIT_CRITICAL(&mux);
        if (m) vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_into_mx, portMAX_DELAY);
}

static void into_mx_give(void) { xSemaphoreGive(s_into_mx); }

/* Whole rows, 1:1 and upright, into a panel buffer: they are one run of
 * memory, and the AXI DMA copies it without the PPA's pass (the PPA at x1
 * was ~48 ms for a 720x1280 frame, the DMA2D copy ~20). Only when the run
 * starts and ends on cache lines, since the DMA sees memory and not the
 * cache. false: not this case, or no DMA; the caller goes on as before. */
static async_memcpy_handle_t s_rows_dma;
static SemaphoreHandle_t s_rows_done;

static bool IRAM_ATTR rows_done_cb(async_memcpy_handle_t mcp, async_memcpy_event_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rows_done, &woken);
    return woken == pdTRUE;
}

static bool rows_dma(uint16_t *fb, int y, int h, const void *src)
{
    const size_t line = 128;
    uint8_t *dst = (uint8_t *)(fb + (size_t)y * AOS_PANEL_W);
    size_t n = (size_t)h * AOS_PANEL_W * 2;
    if (((uintptr_t)dst | n) & (line - 1)) return false;     /* the source: esp_async_memcpy says */
    if (!s_rows_dma) {
        static bool tried;
        if (tried) return false;
        tried = true;
        async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
        cfg.backlog = 4;
        cfg.dma_burst_size = 64;
        s_rows_done = xSemaphoreCreateBinary();
        if (!s_rows_done || esp_async_memcpy_install_gdma_axi(&cfg, &s_rows_dma) != ESP_OK) {
            s_rows_dma = NULL;
            ESP_LOGW(TAG, "rows: no AXI DMA for the copies, the PPA does them");
            return false;
        }
    }
    esp_cache_msync((void *)src, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(dst, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);     /* no stale lines to write back over it */
    into_mx_take();
    xSemaphoreTake(s_rows_done, 0);
    bool ok = esp_async_memcpy(s_rows_dma, dst, (void *)src, n, rows_done_cb, NULL) == ESP_OK &&
              xSemaphoreTake(s_rows_done, pdMS_TO_TICKS(200)) == pdTRUE;
    into_mx_give();
    if (!ok) {
        static int said;
        if (said++ < 3) ESP_LOGW(TAG, "rows: the DMA refused or did not end (src %p), the PPA does it", src);
    }
    return ok;
}

bool aos_hal_display_blit_into(uint16_t *fb, int x, int y, int w, int h, const void *rgb565)
{
    int i = 0;
    while (i < s_nfbs && s_fbs[i] != fb) i++;
    if (i >= s_nfbs || !rgb565 || w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > aos_hal_screen_w() || y + h > aos_hal_screen_h()) return false;
    if (s_rot == 0 && x == 0 && w == AOS_PANEL_W && rows_dma(fb, y, h, rgb565)) return true;
    lv_area_t a = { x, y, x + w - 1, y + h - 1 }, p;
    rotate_area(&a, &p);
    bool ok = false;
    into_mx_take();
    if (blit_ppa_ready()) {
        ppa_srm_rotation_angle_t ang = s_rot == 90  ? PPA_SRM_ROTATION_ANGLE_270 :
                                       s_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                       s_rot == 270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
        ppa_srm_oper_config_t op = {
            .in = {
                .buffer = rgb565, .pic_w = (uint32_t)w, .pic_h = (uint32_t)h,
                .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = fb, .buffer_size = FB_BYTES,
                .pic_w = AOS_PANEL_W, .pic_h = AOS_PANEL_H,
                .block_offset_x = (uint32_t)p.x1, .block_offset_y = (uint32_t)p.y1,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = ang,
            .scale_x = 1.0f,
            .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        ok = ppa_do_scale_rotate_mirror(s_blit_ppa, &op) == ESP_OK;
    }
    into_mx_give();
    if (ok) return true;
    /* the CPU, turned as the flush turns */
    blit_cpu(fb, x, y, w, h, (const uint16_t *)rgb565, 1, false);
    cache_flush(fb + p.y1 * AOS_PANEL_W, (size_t)(p.y2 - p.y1 + 1) * AOS_PANEL_W * 2);
    return true;
}

uint16_t *aos_hal_display_back(void)
{
    if (s_nfbs < 3 || !s_fb) return NULL;
    int req = s_req, scan = s_scan;
    for (int i = 0; i < 3; i++)
        if (i != req && i != scan) return s_fbs[i];
    return NULL;
}

bool aos_hal_display_flip(uint16_t *buf)
{
    int i = 0;
    while (i < s_nfbs && s_fbs[i] != buf) i++;
    if (s_nfbs < 3 || i >= s_nfbs || i == s_req) return false;
    /* after LVGL's copy in flight, so its "done" is not taken for ours */
    for (int64_t t0 = esp_timer_get_time(); s_copy_pending; ) {
        if (esp_timer_get_time() - t0 > 100000) return false;
        vTaskDelay(1);
    }
    /* a buffer of the panel's: esp_lcd writes the cache back and makes it
     * the one the DMA takes at its next refresh, with no copy */
    s_blit_copy = true;
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, AOS_PANEL_W, AOS_PANEL_H, buf);
    s_blit_copy = false;
    if (e != ESP_OK) return false;
    /* only now: set before, a refresh starting in between would mark the
     * old one free while the DMA still scans it */
    s_req = i;
    s_fb = buf;
    s_buf_flip[i] = ++s_flip_seq;
    s_buf_lvgl[i] = false;
    return true;
}

uint32_t aos_hal_display_flips(void) { return s_flip_seq; }

void aos_hal_log_level_set(int level)
{
    esp_log_level_set("*", level <= 1 ? ESP_LOG_ERROR : level == 2 ? ESP_LOG_WARN : ESP_LOG_INFO);
}

bool aos_hal_display_back_age(const uint16_t *fb, uint32_t *flips_since, bool *lvgl_touched)
{
    int i = 0;
    while (i < s_nfbs && s_fbs[i] != fb) i++;
    if (s_nfbs < 3 || i >= s_nfbs) return false;
    if (flips_since) *flips_since = s_buf_flip[i] ? s_flip_seq - s_buf_flip[i] : UINT32_MAX;
    if (lvgl_touched) *lvgl_touched = s_buf_lvgl[i];
    return true;
}

bool aos_hal_display_fb(const uint16_t **px, int *w, int *h)
{
    if (!s_fb) return false;
    /* the DPI and the DMA2D wrote it behind the cache: read it fresh */
    esp_cache_msync(s_fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (px) *px = s_fb;
    if (w) *w = AOS_PANEL_W;
    if (h) *h = AOS_PANEL_H;
    return true;
}

/* aos_hal.h: what the glass shows, upright as the person sees it, to a BMP
 * on the card. From the framebuffer, so the apps that draw straight into it
 * (games, video) come out too. Row by row through one line buffer: no copy
 * of the whole frame, and no long burst on PSRAM (docs/MEMORY.md). */
bool aos_hal_display_save(char *path_out, size_t n)
{
    if (!s_fb || !s_card) return false;
    const int w = aos_hal_screen_w(), h = aos_hal_screen_h();
    /* an album of Photos: the app shows the folders under /photos */
    mkdir(SD_ROOT "/photos", 0777);
    mkdir(SD_ROOT "/photos/Capturas", 0777);
    char path[96];
    struct tm t;
    aos_hal_time_now(&t);
    if (aos_hal_time_is_valid())
        snprintf(path, sizeof path, SD_ROOT "/photos/Capturas/P4OS_%04d%02d%02d_%02d%02d%02d.bmp", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    else
        snprintf(path, sizeof path, SD_ROOT "/photos/Capturas/P4OS_up%lu.bmp", (unsigned long)(esp_timer_get_time() / 1000000));
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t row = (uint32_t)w * 2, img = row * h, off = 14 + 40 + 12;
    uint8_t hd[66] = { 'B', 'M' };
#define LE32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while (0)
    LE32(hd + 2, off + img);
    LE32(hd + 10, off);
    LE32(hd + 14, 40);
    LE32(hd + 18, w);
    LE32(hd + 22, (uint32_t)(-(int32_t)h));     /* rows top-down */
    hd[26] = 1;
    hd[28] = 16;
    LE32(hd + 30, 3);                            /* BI_BITFIELDS: RGB565 */
    LE32(hd + 34, img);
    LE32(hd + 54, 0xF800);
    LE32(hd + 58, 0x07E0);
    LE32(hd + 62, 0x001F);
#undef LE32
    bool ok = fwrite(hd, 1, sizeof hd, f) == sizeof hd;
    uint16_t *line = heap_caps_malloc(row, MALLOC_CAP_SPIRAM);
    ok = ok && line;
    esp_cache_msync(s_fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    for (int y = 0; ok && y < h; y++) {
        for (int x = 0; x < w; x++) line[x] = *fb_at(s_fb, x, y);
        ok = fwrite(line, 1, row, f) == row;
        if ((y & 63) == 63) vTaskDelay(1);
    }
    heap_caps_free(line);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(path); return false; }
    if (path_out && n) snprintf(path_out, n, "%s", path + strlen(SD_ROOT));
    ESP_LOGI(TAG, "screenshot: %s", path);
    return true;
}

/* The src_w x src_h picture (rows stride_px apart) into dst_w x dst_h at the
 * screen's (x, y) of fb, one of the panel's buffers, turned as the screen
 * is: the PPA (factor in 1/16 steps, rounded down so it never spills), or
 * the CPU's nearest neighbour when the PPA refuses. */
static bool fit_into(uint16_t *fb, int x, int y, int dst_w, int dst_h, const void *rgb565, int src_w, int src_h,
                     int stride_px, bool big_endian)
{
    float sx = floorf((float)dst_w * 16 / src_w) / 16, sy = floorf((float)dst_h * 16 / src_h) / 16;
    if (sx <= 0 || sy <= 0) return false;
    int ow = (int)(src_w * sx), oh = (int)(src_h * sy);
    lv_area_t a = { x, y, x + ow - 1, y + oh - 1 }, p;
    rotate_area(&a, &p);
    if (blit_ppa_ready()) {
        ppa_srm_rotation_angle_t ang = s_rot == 90  ? PPA_SRM_ROTATION_ANGLE_270 :
                                       s_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                       s_rot == 270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
        ppa_srm_oper_config_t op = {
            .in = {
                .buffer = rgb565, .pic_w = (uint32_t)stride_px, .pic_h = (uint32_t)src_h,
                .block_w = (uint32_t)src_w, .block_h = (uint32_t)src_h,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = fb, .buffer_size = FB_BYTES,
                .pic_w = AOS_PANEL_W, .pic_h = AOS_PANEL_H,
                .block_offset_x = (uint32_t)p.x1, .block_offset_y = (uint32_t)p.y1,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = ang,
            .scale_x = sx,
            .scale_y = sy,
            .byte_swap = big_endian,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        into_mx_take();
        esp_err_t e = ppa_do_scale_rotate_mirror(s_blit_ppa, &op);
        into_mx_give();
        if (e == ESP_OK) return true;
        if (s_blit_fail_log++ < 4) ESP_LOGW(TAG, "blit: the PPA refused (%s), the CPU does it", esp_err_to_name(e));
    }
    /* nearest neighbour, 16.16 fixed point, turned through fb_at() */
    const uint16_t *src = rgb565;
    const int dx = s_rot == 90 ? AOS_PANEL_W : s_rot == 180 ? -1 : s_rot == 270 ? -AOS_PANEL_W : 1;
    uint32_t fx = ((uint32_t)src_w << 16) / (uint32_t)ow, fy = ((uint32_t)src_h << 16) / (uint32_t)oh;
    for (int oy = 0; oy < oh; oy++) {
        const uint16_t *row = src + (size_t)((oy * fy) >> 16) * stride_px;
        uint16_t *d = fb_at(fb, x, y + oy);
        uint32_t acc = 0;
        for (int ox = 0; ox < ow; ox++, acc += fx, d += dx) {
            uint16_t v = row[acc >> 16];
            *d = big_endian ? (uint16_t)(v << 8 | v >> 8) : v;
        }
    }
    cache_flush(fb + p.y1 * AOS_PANEL_W, (size_t)(p.y2 - p.y1 + 1) * AOS_PANEL_W * 2);
    return true;
}

bool aos_hal_display_blit_fit(int x, int y, int dst_w, int dst_h,
                              const void *rgb565, int src_w, int src_h, int stride_px, bool big_endian)
{
    if (!s_fb || !rgb565 || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || stride_px < src_w) return false;
    if (x < 0 || y < 0 || x + dst_w > aos_hal_screen_w() || y + dst_h > aos_hal_screen_h()) return false;
    for (int64_t t0 = esp_timer_get_time(); s_copy_pending; ) {
        if (esp_timer_get_time() - t0 > 100000) return false;
        vTaskDelay(1);
    }
    return fit_into(s_fb, x, y, dst_w, dst_h, rgb565, src_w, src_h, stride_px, big_endian);
}

bool aos_hal_display_blit_into_fit(uint16_t *fb, int x, int y, int dst_w, int dst_h,
                                   const void *rgb565, int src_w, int src_h, int stride_px, bool big_endian)
{
    int i = 0;
    while (i < s_nfbs && s_fbs[i] != fb) i++;
    if (i >= s_nfbs || !rgb565 || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || stride_px < src_w)
        return false;
    if (x < 0 || y < 0 || x + dst_w > aos_hal_screen_w() || y + dst_h > aos_hal_screen_h()) return false;
    if (s_rot == 0 && !big_endian && x == 0 && src_w == AOS_PANEL_W && dst_w == src_w && dst_h == src_h &&
        stride_px == src_w && rows_dma(fb, y, src_h, rgb565))
        return true;
    return fit_into(fb, x, y, dst_w, dst_h, rgb565, src_w, src_h, stride_px, big_endian);
}

/* The watch's call: its panel took big-endian pixels, 1:1. */
bool aos_hal_display_blit(int x, int y, int w, int h, const void *rgb565_be)
{
    return aos_hal_display_blit_scaled(x, y, w, h, rgb565_be, 1, true);
}

static esp_lcd_touch_handle_t s_touch;

/* The panel's coordinates to the screen's, with the rotation the flush
 * applies (rotate_area) undone. */
static void touch_rotate(int px, int py, int16_t *x, int16_t *y)
{
    switch (s_rot) {
    case 90:  *x = py;                   *y = AOS_PANEL_W - 1 - px; break;
    case 180: *x = AOS_PANEL_W - 1 - px; *y = AOS_PANEL_H - 1 - py; break;
    case 270: *x = AOS_PANEL_H - 1 - py; *y = px;                   break;
    default:  *x = px;                   *y = py;
    }
}

/* Two-finger frames for aos_gesture. The GT911 reports up to five points;
 * the first two are published, already in screen pixels (so
 * aos_ui_touch_map() is the identity on this board). Written and read from
 * the LVGL task only: the indev read and the recogniser's timer. */
#define TOUCH_RING 16
static aos_touch_frame_t s_tf, s_tring[TOUCH_RING];
static uint32_t s_touch_reads, s_touch_samples;

static void touch_publish(int count, const int16_t *x, const int16_t *y)
{
    bool changed = count != s_tf.count;
    for (int i = 0; i < count; i++) changed |= x[i] != s_tf.x[i] || y[i] != s_tf.y[i];
    if (!changed) return;
    s_tf.count = (uint8_t)count;
    for (int i = 0; i < 2; i++) {
        s_tf.x[i] = i < count ? x[i] : 0;
        s_tf.y[i] = i < count ? y[i] : 0;
    }
    s_tf.seq++;
    s_tf.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_tring[s_tf.seq % TOUCH_RING] = s_tf;
    s_touch_samples++;
}

bool aos_hal_touch_frame(aos_touch_frame_t *out)
{
    if (!s_touch) return false;
    *out = s_tf;
    return true;
}

uint32_t aos_hal_touch_frames(uint32_t after_seq, aos_touch_frame_t *out, uint32_t max)
{
    uint32_t last = s_tf.seq, first = after_seq + 1, n = 0;
    if (last >= TOUCH_RING && first <= last - TOUCH_RING) first = last - TOUCH_RING + 1;
    if (last >= first && last - first + 1 > max) first = last - max + 1;
    for (uint32_t q = first; q <= last && q != 0 && n < max; q++) out[n++] = s_tring[q % TOUCH_RING];
    return n;
}

bool aos_hal_touch_multi(void) { return s_touch != NULL; }

void aos_hal_touch_stats(uint32_t *reads, uint32_t *samples)
{
    if (reads) *reads = s_touch_reads;
    if (samples) *samples = s_touch_samples;
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_touch) return;
    esp_lcd_touch_read_data(s_touch);
    s_touch_reads++;
    esp_lcd_touch_point_data_t pt[2];
    uint8_t n = 0;
    esp_lcd_touch_get_data(s_touch, pt, &n, 2);
    int16_t x[2], y[2];
    for (int i = 0; i < n && i < 2; i++) touch_rotate(pt[i].x, pt[i].y, &x[i], &y[i]);
    touch_publish(n > 2 ? 2 : n, x, y);
    if (!n) return;
    data->point.x = x[0];
    data->point.y = y[0];
    data->state = LV_INDEV_STATE_PRESSED;
}

bool aos_hal_lock(uint32_t timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lock, timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void aos_hal_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
int aos_hal_lvgl_core(void) { return 1; }

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 5;
        if (aos_hal_lock(0)) {
            wait = lv_timer_handler();
            aos_hal_unlock();
        }
        if (wait < 1) wait = 1;
        if (wait > 20) wait = 20;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* -------------------------------------------------------------------------- */
/* Start                                                                       */
/* -------------------------------------------------------------------------- */

/* cJSON's trees in PSRAM. Each node is a few dozen bytes, so by
 * SPIRAM_MALLOC_ALWAYSINTERNAL they all went to internal RAM: the portal's
 * listing of a folder of 3000 files (15000 nodes) drained it until
 * esp_hosted could not create a semaphore and an RPC to the C6 failed
 * (2026-09-29). JSON is transient data; the portal, HA, MQTT and Claude all
 * go through here. */
static void *json_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* -------------------------------------------------------------------------- */
/* The hang watchdog (aos_hal.h)                                               */
/* -------------------------------------------------------------------------- */

/* Now and then, after a restart by software (an OTA, the portal's), the
 * board came up without network and, since the USB came, without USB too:
 * unreachable until someone pressed RESET, and a hard reset loses the
 * previous boot's log (2026-09-30). This turns that into a restart by
 * software that keeps it: the boot marks its stages, the main loop beats,
 * and a timer of its own (esp_timer, not the tasks it watches) restarts the
 * board when the boot has not finished 60 s in or the beat has stopped for
 * 30 s, after writing where it was and what each task was doing. At most
 * twice in a row (a counter in noinit PSRAM), so a fault that is always
 * there leaves the board as it is rather than in a loop. */
#define HANG_MAGIC     0x48414E47u          /* "HANG" */
#define HANG_BOOT_MS   60000
#define HANG_BEAT_MS   30000
#define HANG_TRIES     2

static EXT_RAM_NOINIT_ATTR struct { uint32_t magic, count; } s_hang;
static char s_boot_stage[32] = "start";
static volatile uint32_t s_beat_ms;
static volatile bool s_boot_up;
static int s_hang_at_boot;              /* the count as this boot found it: a healthy run clears s_hang */
int aos_hal_hang_restarts(void) { return s_hang_at_boot; }

static esp_timer_handle_t s_hang_timer;

void aos_hal_boot_stage(const char *stage)
{
    snprintf(s_boot_stage, sizeof s_boot_stage, "%s", stage ? stage : "?");
    ESP_LOGI(TAG, "boot stage: %s", s_boot_stage);
}

void aos_hal_alive(void)
{
    s_beat_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_boot_up = true;
}

/* Every task and its state, to the log: what the watchdogs write before
 * they restart the board (this one, and the C6 link's in aos_tasks_p4.c). */
void aos_p4_log_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *ts = heap_caps_calloc(n, sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!ts) return;
    n = uxTaskGetSystemState(ts, n, NULL);
    static const char *const ST[] = { "running", "ready", "blocked", "suspended", "deleted", "invalid" };
    for (UBaseType_t i = 0; i < n; i++)
        ESP_LOGE(TAG, "  task %-16s %-9s prio %u", ts[i].pcTaskName, ST[ts[i].eCurrentState < 6 ? ts[i].eCurrentState : 5],
                 (unsigned)ts[i].uxCurrentPriority);
    heap_caps_free(ts);
}

static void hang_check(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_hang.magic != HANG_MAGIC) { s_hang.magic = HANG_MAGIC; s_hang.count = 0; }
    const char *why = !s_boot_up && now > HANG_BOOT_MS ? "the boot did not finish"
                    : s_boot_up && now - s_beat_ms > HANG_BEAT_MS ? "the main loop stopped beating" : NULL;
    if (!why) {
        if (s_boot_up && now > 3 * HANG_BOOT_MS) s_hang.count = 0;     /* a healthy run clears the tries */
        return;
    }
    ESP_LOGE(TAG, "hang watchdog: %s, %u s after boot, at stage '%s'", why, (unsigned)(now / 1000), s_boot_stage);
    aos_p4_log_tasks();
    esp_timer_stop(s_hang_timer);
    if (s_hang.count >= HANG_TRIES) {
        ESP_LOGE(TAG, "hang watchdog: restarted %d times in a row already; the board stays as it is", HANG_TRIES);
        return;
    }
    s_hang.count++;
    /* out of the cache now: a restart loses what is only there, and the next
     * boot would find the count it started with (2026-10-09: the network's
     * counter never got past "try 1") */
    esp_cache_msync(&s_hang, sizeof s_hang, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    /* abort(), not esp_restart(): the panic writes a core dump with every
     * task's stack, so a hang shows where each one was waiting
     * (tools/coredump.sh), where the list above only says that they were */
    ESP_LOGE(TAG, "hang watchdog: restarting through a panic, for a core dump (try %u of %d); the log above "
             "survives it (GET /api/log?prev=1)", (unsigned)s_hang.count, HANG_TRIES);
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();
}

static void hang_start(void)
{
    s_hang_at_boot = s_hang.magic == HANG_MAGIC ? (int)s_hang.count : 0;
    const esp_timer_create_args_t a = { .callback = hang_check, .name = "hang" };
    if (esp_timer_create(&a, &s_hang_timer) == ESP_OK) esp_timer_start_periodic(s_hang_timer, 5 * 1000 * 1000);
}

/* aos_hal.h: the tuning preferences (GET/POST /api/tune) back to their
 * defaults - what the safe boot and the BOOT button's safe mode do. */
void aos_hal_tune_reset(void)
{
    static const char *const TUNE[] = { "lvbuf", "lvrows", "fbs", "blit_hw", "bands_psram" };
    for (size_t i = 0; i < sizeof TUNE / sizeof TUNE[0]; i++) aos_hal_pref_erase(TUNE[i]);
}

/* -------------------------------------------------------------------------- */
/* The BOOT button (aos_hal.h, "Physical buttons")                             */
/* -------------------------------------------------------------------------- */

/* GPIO35: a strapping pin with its own 4.7 k pull-up (HARDWARE.md), low
 * while pressed. Held at reset it sends the chip to the ROM's download
 * mode, so the firmware only ever sees it after boot. The POWER button
 * reaches no GPIO at all (a power controller: short on, 2 s off). Polled
 * every 10 ms by a small thread of its own rather than an interrupt: two
 * equal samples in a row are a change (debounce), and the callback runs
 * there, where it may take the LVGL lock and write a file. */
#define BOOT_GPIO       35
#define BUTTON_LONG_MS  800

static void (*s_button_cb)(aos_button_t button, aos_button_action_t action);
static volatile bool s_boot_down;

void aos_hal_set_button_cb(void (*cb)(aos_button_t button, aos_button_action_t action)) { s_button_cb = cb; }
bool aos_hal_button_is_down(aos_button_t button) { return button == AOS_BUTTON_BOOT && s_boot_down; }

static void button_thread(void *arg)
{
    (void)arg;
    int stable = 1, last = 1;
    uint32_t t_down = 0;
    for (;;) {
        int lv = gpio_get_level(BOOT_GPIO);
        if (lv == last && lv != stable) {
            stable = lv;
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (!lv) {
                s_boot_down = true;
                t_down = now;
                ESP_LOGI(TAG, "BOOT: pressed");
                if (s_button_cb) s_button_cb(AOS_BUTTON_BOOT, AOS_BUTTON_PRESS);
            } else {
                s_boot_down = false;
                ESP_LOGI(TAG, "BOOT: released after %u ms", (unsigned)(now - t_down));
                if (s_button_cb)
                    s_button_cb(AOS_BUTTON_BOOT, now - t_down >= BUTTON_LONG_MS ? AOS_BUTTON_LONG : AOS_BUTTON_CLICK);
            }
        }
        last = lv;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void button_start(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    if (gpio_config(&io) != ESP_OK) return;
    s_boot_down = gpio_get_level(BOOT_GPIO) == 0;
    ESP_LOGI(TAG, "BOOT button on GPIO%d: %s", BOOT_GPIO, s_boot_down ? "held" : "up");
    /* 6 KB in PSRAM: the screenshot of a long press is written from here */
    aos_hal_thread_start("button", button_thread, NULL, 6144, 4);
}

bool aos_hal_init(void)
{
    s_log_prev = esp_log_set_vprintf(log_hook);
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    void aos_flashop_init(void);
    aos_flashop_init();             /* before the first thread with its stack in PSRAM */
    hang_start();
    /* Safe boot. The tuning preferences (GET/POST /api/tune: lvbuf, lvrows,
     * fbs, blit_hw) are read at boot, and a wrong one can keep the board
     * from coming up at all: on 2026-09-30 one draw buffer in internal RAM
     * left it dead, the preference kept, every boot the same. So each boot
     * counts itself in "boot_bad" and main.c clears it once the system has
     * been up 30 s; two boots that never got there, and the third one
     * forgets the tuning. A counter in NVS and not in RAM: a hang only
     * ends with the power, which RAM does not survive. */
    {
        int32_t bad = 0;
        aos_hal_pref_get_i32("boot_bad", &bad);
        if (bad >= 2) {
            aos_hal_tune_reset();
            ESP_LOGE(TAG, "safe boot: %d boots in a row did not get to 30 s up; the tuning preferences are forgotten",
                     (int)bad);
            bad = 0;
        }
        aos_hal_pref_set_i32("boot_bad", bad + 1);
    }
    button_start();
    char tz[64];
    if (aos_hal_pref_get_str("tz", tz, sizeof tz)) snprintf(s_tz, sizeof s_tz, "%s", tz);
    setenv("TZ", s_tz, 1);
    tzset();

    aos_hal_boot_stage("card");
    sd_mount();
    aos_hal_boot_stage("panel");
    panel_init();

    s_lock = xSemaphoreCreateRecursiveMutex();

    aos_hal_boot_stage("lvgl");
    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(AOS_PANEL_W, AOS_PANEL_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    /* the same bytes either way up: LVGL makes rows of them for the width in use */
    size_t bytes = DRAW_BYTES;
    void *b1 = NULL, *b2 = NULL;
    /* Where LVGL draws (internal RAM audit, docs/MEMORY.md): pref "lvbuf"
     * 0 two buffers in internal RAM, 1 one in internal RAM, 2 two in PSRAM
     * (the default), 3 one in PSRAM; "lvrows" their height upright (128 by
     * default). Read at boot; GET /api/display/bench measures a choice.
     *
     * Measured on the board (2026-09-30), the whole screen redrawn:
     * two internal x 56 rows 67.0 ms upright / 90.5 lying down; two in
     * PSRAM x 56 rows 78.1; x 128 66.7 / 83.3; x 320 60.3 / 77.8. Fewer,
     * taller chunks pay the per-chunk cost (setting up the copy, waiting
     * for the DMA) fewer times, which more than makes up for PSRAM; and the
     * 161 KB of internal RAM are free. 320 rows is faster still (+0.55 MB
     * of PSRAM), but Monster Hop, the hungriest app, then has 320 KB of
     * PSRAM left at its worst moment (880 KB with 128): its map once failed
     * to load for want of a block (fixed in the app, mh_ui.c). 128 until
     * the apps need less. */
    int32_t lvbuf = 2, lvrows = 128;
    aos_hal_pref_get_i32("lvbuf", &lvbuf);
    aos_hal_pref_get_i32("lvrows", &lvrows);
    if (lvbuf < 0 || lvbuf > 3) lvbuf = 2;
    if (lvrows < 16 || lvrows > AOS_PANEL_H) lvrows = 128;
    const bool two = lvbuf == 0 || lvbuf == 2, in_psram = lvbuf >= 2;
    /* internal RAM first (PSRAM renders slower), but never so much that the
     * drivers that come after go without: on the board 2 x 56 rows left the
     * I2S and the C6's SDIO unable to get their DMA memory */
    const size_t RESERVE = 140 * 1024;
    /* Up to 56 rows (2 x 80 KB). 40 rows (tried 2026-09-29) freed 46 KB of
     * internal RAM for apps' bands and let the rotation buffer exist lying
     * down, but the retro canvas is scaled by the PPA once per chunk LVGL
     * refreshes, and 2043 fell from 20.7 to 17.9 fps presenting its whole
     * canvas (23.7 with its changed tiles at 56 rows, 21.3 at 40;
     * 2026-09-30). The games that present by themselves still get bands of
     * 17 rows upright. */
    if (!in_psram) {
        for (int rows = (int)lvrows; rows >= 24 && !b1; rows -= 8) {
            bytes = (size_t)AOS_PANEL_W * rows * 2;
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < (two ? 2 : 1) * bytes + RESERVE) continue;
            b1 = heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            b2 = b1 && two ? heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : NULL;
            if (two && !b2) { heap_caps_free(b1); b1 = NULL; }
        }
    }
    if (!b1) {
        heap_caps_free(b2);
        bytes = (size_t)AOS_PANEL_W * lvrows * 2;
        b1 = heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM);
        b2 = two ? heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM) : NULL;
        if (!in_psram) ESP_LOGW(TAG, "draw buffers in PSRAM: no internal RAM for them");
    }
    s_draw_bytes = bytes;
    s_draw_mode = (int)lvbuf;
    s_draw_bufs = b2 ? 2 : 1;
    ESP_LOGI(TAG, "draw buffers: %d x %u bytes (%u rows upright) in %s; internal free %u, largest %u", b2 ? 2 : 1,
             (unsigned)bytes, (unsigned)(bytes / (AOS_PANEL_W * 2)),
             esp_ptr_external_ram(b1) ? "PSRAM" : "internal RAM", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lv_display_set_buffers(s_disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    bsp_display_cfg_t tcfg = { 0 };
    aos_hal_boot_stage("touch");
    if (bsp_touch_new(&tcfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 not found");
        s_touch = NULL;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    /* Internal stack: LVGL is the hottest task, and it runs the apps' event
     * callbacks. 7,9 KB at the deepest on the board with the shell and the
     * apps opened so far (2026-09-29), so 12 KB and not 16. */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "P4 HAL up: panel, LVGL, touch %s, microSD %s", s_touch ? "ok" : "MISSING",
             s_card ? "ok" : "none");
    extern void aos_net_p4_start(void);
    aos_hal_boot_stage("network");
    aos_net_p4_start();             /* the radio comes up on its own task */
    extern void aos_audio_p4_start(void);
    aos_hal_boot_stage("audio");
    aos_audio_p4_start();           /* the codecs come up on the audio task (aos_audio_p4.c) */
    extern void aos_tasks_p4_start(void);
    aos_tasks_p4_start();           /* the Monitor's sampler: CPU, memory, temperature (aos_tasks_p4.c) */
    return true;
}

/* -------------------------------------------------------------------------- */
/* Threads and locks for services                                              */
/* -------------------------------------------------------------------------- */

/* The apps' threads (portal connections, HA, MQTT, Claude, the bench, the
 * file jobs...) get their stacks in PSRAM: they only talk to the network,
 * the card and the UART, and the preferences reach the flash through
 * aos_flash_call. If PSRAM has no room left they fall back to internal RAM. */
typedef struct { void (*fn)(void *); void *arg; bool psram; } thread_arg_t;

static void thread_main(void *p)
{
    thread_arg_t t = *(thread_arg_t *)p;
    free(p);
    t.fn(t.arg);
    if (t.psram) vTaskDeleteWithCaps(NULL);     /* frees the PSRAM stack too */
    else vTaskDelete(NULL);                     /* a FreeRTOS task must not return */
}

bool aos_hal_thread_start(const char *name, void (*fn)(void *arg), void *arg, uint32_t stack_bytes, int prio)
{
    thread_arg_t *t = malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
    if (prio < 1) prio = 1;
    if (prio > 10) prio = 10;
    t->psram = true;
    if (xTaskCreatePinnedToCoreWithCaps(thread_main, name, stack_bytes, t, prio, NULL, 0,
                                        MALLOC_CAP_SPIRAM) == pdPASS) return true;
    t->psram = false;
    if (xTaskCreatePinnedToCore(thread_main, name, stack_bytes, t, prio, NULL, 0) == pdPASS) return true;
    free(t);
    return false;
}

void aos_hal_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
void *aos_hal_mutex_create(void) { return xSemaphoreCreateRecursiveMutex(); }
void aos_hal_mutex_lock(void *m) { xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY); }
void aos_hal_mutex_unlock(void *m) { xSemaphoreGiveRecursive((SemaphoreHandle_t)m); }
