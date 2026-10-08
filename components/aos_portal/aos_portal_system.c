/*
 * P4OS - firmware updates over Wi-Fi, the core dump and the internal heap,
 * from the portal.
 *
 *   GET  /api/ota                 the running slot, whether it is on trial, the
 *                                 version, and the other slot's image, if any
 *   PUT  /api/ota                 the body is a firmware .bin (build/<profile>/
 *                                 p4os.bin): written into the idle slot as it
 *                                 arrives, checked, made the boot one
 *   POST /api/ota/restart         restarts into it (it boots on trial: main.c
 *                                 confirms it after 30 s up, else the previous
 *                                 image comes back by itself)
 *   POST /api/ota/other           boots the other slot at the next restart
 *   GET  /api/coredump            the last panic's summary (task, pc, ra, cause),
 *                                 which firmware made it (elf_sha, slot, version),
 *                                 when it was first seen and whether it is unread
 *   GET  /api/coredump/elf        the dump itself, for tools/coredump.sh (read
 *                                 whole, it counts as read)
 *   POST /api/coredump/read       counts it as read
 *   POST /api/coredump/erase      forgets it
 *   POST /api/coredump/test?ms=   development: a panic on purpose, to try all
 *                                 of the above (the board restarts)
 *   GET  /api/display/bench?frames=   LVGL redrawing the whole screen, ms a frame
 *   GET  /api/tune   POST /api/tune?key=&value=   the tuning preferences read
 *                                 at boot (lvbuf, lvrows, fbs, blit_hw,
 *                                 bands_psram); no
 *                                 value erases one
 *   GET  /api/heap                the internal heap block by block: the big
 *                                 ones named after the task whose stack they
 *                                 are, the rest as a histogram by size
 *   GET  /api/heap?trace=1        on a diagnostic build, who made each live
 *                                 internal allocation (tools/heap_owners.py)
 *
 * tools/ota.sh builds, uploads, restarts and checks that the slot changed.
 */
#include "aos_portal_system.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif
#endif

#define CHUNK 16384

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void send_err(aos_httpd_req_t *r, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    send_cjson(r, status, o);
}

static void api_ota_status(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "running", aos_hal_ota_running_slot());
    cJSON_AddBoolToObject(o, "trial", aos_hal_ota_pending_verify());
    cJSON_AddStringToObject(o, "version", aos_hal_firmware_version());
#ifdef ESP_PLATFORM
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON_AddStringToObject(o, "built", d->date);
    cJSON_AddStringToObject(o, "time", d->time);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    /* the name tools/elf_keep.sh keeps this image's ELF under, in build/elf/ */
    char sha[17];                       /* esp_app_get_elf_sha256 stops at 9 */
    for (int i = 0; i < 8; i++) snprintf(sha + 2 * i, 3, "%02x", d->app_elf_sha256[i]);
    cJSON_AddStringToObject(o, "elf", sha);
#endif
    aos_ota_info_t oi;
    if (aos_hal_ota_info(&oi) && oi.other_slot[0]) {
        cJSON *x = cJSON_AddObjectToObject(o, "other");
        cJSON_AddStringToObject(x, "slot", oi.other_slot);
        cJSON_AddNumberToObject(x, "size", oi.other_size);
        if (oi.other_version[0]) {
            cJSON_AddStringToObject(x, "version", oi.other_version);
            cJSON_AddStringToObject(x, "built", oi.other_built);
            cJSON_AddStringToObject(x, "elf", oi.other_elf);
        }
        if (oi.other_state[0]) cJSON_AddStringToObject(x, "state", oi.other_state);
    }
    if (oi.flash_id) {
        char id[12];
        snprintf(id, sizeof id, "0x%06x", (unsigned)oi.flash_id);
        cJSON_AddStringToObject(o, "flash_id", id);
    }
    send_cjson(r, 200, o);
}

static void api_ota_put(aos_httpd_req_t *r)
{
    long len = aos_httpd_body_len(r);
    if (len <= 0) { send_err(r, 400, "falta Content-Length"); return; }
    if (!aos_hal_ota_begin((size_t)len)) { send_err(r, 409, aos_hal_ota_error()); return; }
    char *buf = malloc(CHUNK);
    if (!buf) { aos_hal_ota_abort(); send_err(r, 500, "sin memoria"); return; }
    long done = 0;
    int n = 0;
    uint64_t t0 = aos_hal_uptime_ms();
    while ((n = aos_httpd_body_read(r, buf, CHUNK)) > 0) {
        if (!aos_hal_ota_write(buf, (size_t)n)) break;
        done += n;
    }
    free(buf);
    if (done != len) {
        aos_hal_ota_abort();
        char m[96];
        snprintf(m, sizeof m, "se cortó la subida: %ld de %ld B (%s)", done, len, aos_hal_ota_error());
        send_err(r, 500, m);
        return;
    }
    if (!aos_hal_ota_end()) { send_err(r, 422, aos_hal_ota_error()); return; }
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    aos_hal_log("ota", "image of %ld B in %u ms, waiting for the restart", done, (unsigned)ms);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "size", (double)done);
    cJSON_AddNumberToObject(o, "ms", ms);
    cJSON_AddStringToObject(o, "restart", "/api/ota/restart");
    send_cjson(r, 200, o);
}

static void restart_soon(void *arg)
{
    (void)arg;
    aos_hal_sleep_ms(300);                  /* the answer goes out first */
    aos_hal_reboot();
}

static void api_coredump(aos_httpd_req_t *r)
{
    aos_coredump_info_t ci;
    cJSON *o = cJSON_CreateObject();
    bool ok = aos_hal_coredump_info(&ci);
    cJSON_AddBoolToObject(o, "present", ok && ci.present);
    if (ok && ci.present) {
        char h[16];
        cJSON_AddBoolToObject(o, "valid", ci.valid);
        cJSON_AddNumberToObject(o, "size", (double)ci.size);
        cJSON_AddStringToObject(o, "task", ci.task);
#define HEX(k, v) do { snprintf(h, sizeof h, "0x%08lx", (unsigned long)(v)); cJSON_AddStringToObject(o, k, h); } while (0)
        HEX("pc", ci.pc);
        HEX("ra", ci.ra);
        HEX("sp", ci.sp);
        HEX("mcause", ci.mcause);
        HEX("mtval", ci.mtval);
#undef HEX
        cJSON_AddStringToObject(o, "elf_sha", ci.elf_sha);
        cJSON_AddStringToObject(o, "slot", ci.slot);
        cJSON_AddStringToObject(o, "version", ci.version);
        cJSON_AddNumberToObject(o, "seen", (double)ci.seen);
        cJSON_AddBoolToObject(o, "unread", ci.unread);
    }
    send_cjson(r, 200, o);
}

static void api_coredump_elf(aos_httpd_req_t *r)
{
    aos_coredump_info_t ci;
    if (!aos_hal_coredump_info(&ci) || !ci.present) { send_err(r, 404, "no hay volcado"); return; }
    char *buf = malloc(CHUNK);
    if (!buf) { send_err(r, 500, "sin memoria"); return; }
    char disp[96];
    snprintf(disp, sizeof disp, "Content-Disposition: attachment; filename=\"p4os-coredump-%s.elf\"\r\n",
             ci.elf_sha[0] ? ci.elf_sha : "unknown");
    size_t off = 0;
    if (aos_httpd_begin(r, 200, "application/octet-stream", (long)ci.size, disp)) {
        while (off < ci.size) {
            size_t n = aos_hal_coredump_read(off, buf, CHUNK);
            if (!n || !aos_httpd_write(r, buf, n)) break;
            off += n;
        }
    }
    free(buf);
    if (off >= ci.size) aos_hal_coredump_mark_read();      /* taken whole: read */
}

#ifdef ESP_PLATFORM
/* GET /api/heap: who holds the internal RAM, block by block (docs/MEMORY.md,
 * "Internal RAM audit"). heap_caps_walk() holds the heap's lock while it
 * calls back, so the callback only counts into arrays that exist already;
 * the blocks of 1 KB and more are named after the task whose stack they
 * are, when they are one. The rest is a histogram by size. */
#define HW_BIG_MAX 96
typedef struct {
    struct { uintptr_t ptr; uint32_t size; } big[HW_BIG_MAX];
    int nbig, dropped;
    uint32_t count[7], bytes[7];    /* <=32, <=64, <=128, <=256, <=512, <1K, big */
    uint32_t used, free_, largest_free;
} heap_walk_t;

static bool heap_walk_cb(walker_heap_into_t h, walker_block_info_t b, void *user)
{
    (void)h;
    heap_walk_t *w = user;
    if (!b.used) {
        w->free_ += b.size;
        if (b.size > w->largest_free) w->largest_free = b.size;
        return true;
    }
    w->used += b.size;
    int k = b.size <= 32 ? 0 : b.size <= 64 ? 1 : b.size <= 128 ? 2 : b.size <= 256 ? 3 : b.size <= 512 ? 4
          : b.size < 1024 ? 5 : 6;
    w->count[k]++;
    w->bytes[k] += b.size;
    if (k == 6) {
        if (w->nbig < HW_BIG_MAX) { w->big[w->nbig].ptr = (uintptr_t)b.ptr; w->big[w->nbig].size = b.size; w->nbig++; }
        else w->dropped++;
    }
    return true;
}

#if CONFIG_HEAP_TRACING_STANDALONE
/* ?trace=1 on a diagnostic build (main.c starts the trace): the live
 * allocations in internal RAM, grouped by who made them. The callers are
 * code addresses; tools/heap_owners.py turns them into function names. */
#define HT_GROUPS 160
static void api_heap_trace(aos_httpd_req_t *r)
{
    typedef struct { void *by[CONFIG_HEAP_TRACING_STACK_DEPTH]; uint32_t n, bytes; } grp_t;
    grp_t *g = heap_caps_calloc(HT_GROUPS, sizeof *g, MALLOC_CAP_SPIRAM);
    if (!g) { send_err(r, 500, "memory"); return; }
    int ng = 0;
    uint32_t other_n = 0, other_bytes = 0;
    size_t count = heap_trace_get_count();
    for (size_t i = 0; i < count; i++) {
        heap_trace_record_t rec;
        if (heap_trace_get(i, &rec) != ESP_OK || !rec.address || rec.freed) continue;
        if (!esp_ptr_internal(rec.address)) continue;
        int k = 0;
        while (k < ng && memcmp(g[k].by, rec.alloced_by, sizeof g[k].by)) k++;
        if (k == ng) {
            if (ng == HT_GROUPS) { other_n++; other_bytes += rec.size; continue; }
            memcpy(g[k].by, rec.alloced_by, sizeof g[k].by);
            ng++;
        }
        g[k].n++;
        g[k].bytes += rec.size;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "records", count);
    cJSON *arr = cJSON_AddArrayToObject(o, "owners");
    for (int k = 0; k < ng; k++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "blocks", g[k].n);
        cJSON_AddNumberToObject(e, "bytes", g[k].bytes);
        cJSON *by = cJSON_AddArrayToObject(e, "by");
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH; d++) {
            char a[12];
            snprintf(a, sizeof a, "0x%08x", (unsigned)(uintptr_t)g[k].by[d]);
            cJSON_AddItemToArray(by, cJSON_CreateString(a));
        }
        cJSON_AddItemToArray(arr, e);
    }
    if (other_n) { cJSON_AddNumberToObject(o, "ungrouped_blocks", other_n); cJSON_AddNumberToObject(o, "ungrouped_bytes", other_bytes); }
    free(g);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    aos_httpd_send_json(r, 200, str ? str : "{}");
    free(str);
}
#endif

static void api_heap(aos_httpd_req_t *r)
{
#if CONFIG_HEAP_TRACING_STANDALONE
    if (aos_httpd_query_int(r, "trace", 0)) { api_heap_trace(r); return; }
#endif
    heap_walk_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
    UBaseType_t nt = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *ts = heap_caps_calloc(nt, sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!w || !ts) { free(w); free(ts); send_err(r, 500, "memory"); return; }
    nt = uxTaskGetSystemState(ts, nt, NULL);
    heap_caps_walk(MALLOC_CAP_INTERNAL, heap_walk_cb, w);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "used", w->used);
    cJSON_AddNumberToObject(o, "free", w->free_);
    cJSON_AddNumberToObject(o, "largest_free", w->largest_free);
    static const char *const K[7] = { "<=32", "<=64", "<=128", "<=256", "<=512", "<1K", ">=1K" };
    cJSON *hist = cJSON_AddArrayToObject(o, "sizes");
    for (int i = 0; i < 7; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "size", K[i]);
        cJSON_AddNumberToObject(e, "blocks", w->count[i]);
        cJSON_AddNumberToObject(e, "bytes", w->bytes[i]);
        cJSON_AddItemToArray(hist, e);
    }
    cJSON *big = cJSON_AddArrayToObject(o, "big");
    for (int i = 0; i < w->nbig; i++) {
        cJSON *e = cJSON_CreateObject();
        char a[12];
        snprintf(a, sizeof a, "0x%08x", (unsigned)w->big[i].ptr);
        cJSON_AddStringToObject(e, "ptr", a);
        cJSON_AddNumberToObject(e, "size", w->big[i].size);
        /* a task's stack: the block holds pxStackBase */
        for (UBaseType_t t = 0; t < nt; t++) {
            uintptr_t base = (uintptr_t)ts[t].pxStackBase;
            if (base >= w->big[i].ptr && base < w->big[i].ptr + w->big[i].size) {
                cJSON_AddStringToObject(e, "stack", ts[t].pcTaskName);
                break;
            }
        }
        cJSON_AddItemToArray(big, e);
    }
    if (w->dropped) cJSON_AddNumberToObject(o, "big_not_listed", w->dropped);
    free(w);
    free(ts);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    aos_httpd_send_json(r, 200, str ? str : "{}");
    free(str);
}
#endif

/* GET /api/display/bench?frames=: LVGL redrawing the whole screen. */
static void api_display_bench(aos_httpd_req_t *r)
{
    aos_display_bench_t b;
    if (!aos_hal_display_bench((int)aos_httpd_query_int(r, "frames", 30), &b)) { send_err(r, 503, "no display"); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "frames", b.frames);
    cJSON_AddNumberToObject(o, "ms_per_frame", b.us_per_frame / 1000.0);
    cJSON_AddNumberToObject(o, "fps", b.us_per_frame ? 1e6 / b.us_per_frame : 0);
    cJSON_AddNumberToObject(o, "lvbuf", b.mode);
    cJSON_AddNumberToObject(o, "rows", b.rows);
    cJSON_AddNumberToObject(o, "buffers", b.buffers);
    send_cjson(r, 200, o);
}

/* GET /api/tune, POST /api/tune?key=&value=: the preferences that tune the
 * system and are read at boot, a closed list (docs/MEMORY.md). */
static const char *const TUNE_KEYS[] = { "lvbuf", "lvrows", "fbs", "blit_hw", "bands_psram" };

static void api_tune(aos_httpd_req_t *r, bool set)
{
    if (set) {
        char key[16] = "";
        aos_httpd_query(r, "key", key, sizeof key);
        bool known = false;
        for (size_t i = 0; i < sizeof TUNE_KEYS / sizeof TUNE_KEYS[0]; i++) known |= !strcmp(key, TUNE_KEYS[i]);
        if (!known) { send_err(r, 400, "clave desconocida"); return; }
        char v[16] = "";
        if (!aos_httpd_query(r, "value", v, sizeof v)) { aos_hal_pref_erase(key); }
        else if (!aos_hal_pref_set_i32(key, (int32_t)strtol(v, NULL, 10))) { send_err(r, 500, "no se pudo guardar"); return; }
    }
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < sizeof TUNE_KEYS / sizeof TUNE_KEYS[0]; i++) {
        int32_t v;
        if (aos_hal_pref_get_i32(TUNE_KEYS[i], &v)) cJSON_AddNumberToObject(o, TUNE_KEYS[i], v);
        else cJSON_AddNullToObject(o, TUNE_KEYS[i]);
    }
    cJSON_AddStringToObject(o, "note", "se leen al arrancar: reiniciar para aplicar");
    send_cjson(r, 200, o);
}

bool aos_portal_system(aos_httpd_req_t *r, const char *m, const char *p)
{
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST"), put = !strcmp(m, "PUT");
    if (get && !strcmp(p, "ota")) api_ota_status(r);
    else if (put && !strcmp(p, "ota")) api_ota_put(r);
    else if (post && !strcmp(p, "ota/restart")) {
        aos_httpd_send_json(r, 200, "{\"ok\":true}");
        aos_hal_thread_start("ota_rst", restart_soon, NULL, 3072, 5);
    } else if (post && !strcmp(p, "ota/other")) {
        if (aos_hal_ota_boot_other()) aos_httpd_send_json(r, 200, "{\"ok\":true}");
        else send_err(r, 409, aos_hal_ota_error()[0] ? aos_hal_ota_error() : "la otra ranura no tiene una imagen");
    } else if (get && !strcmp(p, "coredump")) api_coredump(r);
    else if (get && !strcmp(p, "coredump/elf")) api_coredump_elf(r);
    else if (get && !strcmp(p, "display/bench")) api_display_bench(r);
    else if ((get || post) && !strcmp(p, "tune")) api_tune(r, post);
#ifdef ESP_PLATFORM
    else if (get && !strcmp(p, "heap")) api_heap(r);
#endif
    else if (post && !strcmp(p, "coredump/read")) {
        aos_hal_coredump_mark_read();
        aos_httpd_send_json(r, 200, "{\"ok\":true}");
    } else if (post && !strcmp(p, "coredump/test")) {
        char v[16] = "";
        uint32_t ms = aos_httpd_query(r, "ms", v, sizeof v) ? (uint32_t)atoi(v) : 500;
        aos_hal_log("coredump", "a panic on purpose asked for from the portal, in %u ms", (unsigned)ms);
        aos_httpd_send_json(r, 200, "{\"ok\":true}");
        aos_hal_coredump_test_panic(ms < 200 ? 200 : ms);
    } else if (post && !strcmp(p, "coredump/erase")) {
        if (aos_hal_coredump_erase()) aos_httpd_send_json(r, 200, "{\"ok\":true}");
        else send_err(r, 500, "no se pudo borrar");
    } else return false;
    return true;
}
