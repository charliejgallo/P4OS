/*
 * P4OS HAL - what the Monitor shows (aos.sysmon, /api/sysmon): the FreeRTOS
 * task list, heap_caps by capability and the system's facts. See the
 * "Monitor" block of aos_hal.h.
 *
 * It also runs the "stats" task, which the P4 needs because it has no
 * housekeeping task like the watch had: it calls aos_stats_tick() (the CPU
 * load per core, memory, the chip's temperature and their hour of history,
 * aos_stats.c), refreshes the task list every few seconds so the 64-bit run
 * totals never miss a wrap of the 32-bit counters, and watches the Wi-Fi
 * link to know how long it has been up.
 *
 * The CPU per task is the run-time counter's advance (CONFIG_FREERTOS_
 * GENERATE_RUN_TIME_STATS, microseconds from esp_timer) over the wall time
 * of the window, as a percentage of ONE core. A refresh less than half a
 * second after the window opened reuses the last figures, so the app and
 * the portal asking at once do not shrink each other's window to nothing.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_private/freertos_idf_additions_priv.h"
#include "esp_private/freertos_debug.h"
#include "esp_private/esp_clk.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_attr.h"

static const char *TAG = "tasks";

/* Room for more tasks than aos_hal_tasks() hands out: uxTaskGetSystemState()
 * returns nothing at all when the array is short. */
#define TASKS_CAP (AOS_TASKS_MAX + 32)

void aos_stats_tick(void);          /* aos_stats.c */

/* -------------------------------------------------------------------------- */
/* Task list                                                                   */
/* -------------------------------------------------------------------------- */

/* What is kept per task between refreshes, by task number. */
typedef struct {
    UBaseType_t num;
    uint32_t    raw;            /* the 32-bit counter last seen */
    uint64_t    total;          /* its 64-bit total */
    uint64_t    win_total;      /* the total when the current window opened */
    int16_t     cpu_x10;        /* the figure of the last closed window, -1 none yet */
    int32_t     mark;           /* stack high-water mark in bytes, -1 not measured yet */
    bool        seen;
} track_t;

static SemaphoreHandle_t s_mx;
static TaskStatus_t *s_st;          /* PSRAM, TASKS_CAP of each */
static track_t      *s_tr;
static int           s_ntr;
static int64_t       s_win_us;      /* when the current window opened */

static bool tasks_init(void)
{
    static bool tried;
    if (!tried) {
        tried = true;
        s_mx = xSemaphoreCreateMutex();
        s_st = heap_caps_calloc(TASKS_CAP, sizeof *s_st, MALLOC_CAP_SPIRAM);
        s_tr = heap_caps_calloc(TASKS_CAP, sizeof *s_tr, MALLOC_CAP_SPIRAM);
        if (!s_mx || !s_st || !s_tr) ESP_LOGE(TAG, "no memory for the task list");
    }
    return s_mx && s_st && s_tr;
}

static track_t *track_find(UBaseType_t num)
{
    for (int i = 0; i < s_ntr; i++) if (s_tr[i].num == num) return &s_tr[i];
    return NULL;
}

static uint8_t state_of(eTaskState s)
{
    switch (s) {
    case eRunning:   return AOS_TASK_RUNNING;
    case eReady:     return AOS_TASK_READY;
    case eBlocked:   return AOS_TASK_BLOCKED;
    case eSuspended: return AOS_TASK_SUSPENDED;
    case eDeleted:   return AOS_TASK_DELETED;
    default:         return AOS_TASK_UNKNOWN;
    }
}

/* Snapshot into s_st, totals forward and, when the window is old enough,
 * each task's CPU. With s_mx held. Returns how many tasks s_st holds. */
static int refresh_locked(void)
{
    /* Not uxTaskGetSystemState(): it measures every task's high-water mark
     * by reading its whole stack, and with stacks in PSRAM that burst of
     * reads starved the panel's refresh - a white flash every time this ran
     * (on the board, 2026-09-29). The snapshot plus vTaskGetInfo without the
     * stack check gives the same list; the mark is read here for every stack
     * in internal RAM, which the panel does not share, and for one stack in
     * PSRAM per call, taking turns: a few KB, not the whole lot at once.
     *
     * Under the kernel's lock, not vTaskSuspendAll(): that stops only this
     * core's scheduler, and a portal thread deleting itself on the other
     * core freed its stack under this loop - a NULL stack and a panic during
     * a long upload (2026-09-29). With the lock held no task can leave the
     * lists. A task that is suspended may be one of those deleting
     * themselves (vTaskDeleteWithCaps), so its stack is left alone. */
    static TaskSnapshot_t snap[TASKS_CAP];
    static bool known[TASKS_CAP];
    static unsigned turn;           /* which PSRAM stack gets measured this time */
    unsigned in_psram = 0;
    UBaseType_t tcb_size;
    prvTakeKernelLock();
    UBaseType_t n = uxTaskGetSnapshotAll(snap, TASKS_CAP, &tcb_size);
    for (UBaseType_t k = 0; k < n; k++) {
        TaskHandle_t h = (TaskHandle_t)snap[k].pxTCB;
        vTaskGetInfo(h, &s_st[k], pdFALSE, eInvalid);
        bool readable = s_st[k].pxStackBase && s_st[k].eCurrentState != eSuspended &&
                        s_st[k].eCurrentState != eDeleted;
        known[k] = readable && (!esp_ptr_external_ram(s_st[k].pxStackBase) || in_psram++ == turn);
        s_st[k].usStackHighWaterMark = known[k] ? uxTaskGetStackHighWaterMark(h) : 0;
    }
    prvReleaseKernelLock();
    turn = in_psram ? (turn + 1) % in_psram : 0;
    int64_t now = esp_timer_get_time();
    int64_t span = now - s_win_us;
    bool close = !s_win_us || span >= 500000;

    for (int i = 0; i < s_ntr; i++) s_tr[i].seen = false;
    for (UBaseType_t k = 0; k < n; k++) {
        const TaskStatus_t *t = &s_st[k];
        uint32_t raw = (uint32_t)t->ulRunTimeCounter;
        track_t *tr = track_find(t->xTaskNumber);
        if (!tr) {
            if (s_ntr >= TASKS_CAP) continue;
            tr = &s_tr[s_ntr++];
            *tr = (track_t){ .num = t->xTaskNumber, .raw = raw, .total = raw, .win_total = raw,
                             .cpu_x10 = -1, .mark = -1 };
        }
        tr->seen = true;
        if (known[k]) tr->mark = (int32_t)t->usStackHighWaterMark;     /* bytes: StackType_t is uint8_t */
        tr->total += (uint32_t)(raw - tr->raw);     /* the counter wraps every 71 min */
        tr->raw = raw;
        if (close) {
            if (s_win_us && span > 0) {
                int64_t x = (int64_t)(tr->total - tr->win_total) * 1000 / span;
                tr->cpu_x10 = (int16_t)(x < 0 ? 0 : x > 1000 ? 1000 : x);
            }
            tr->win_total = tr->total;
        }
    }
    if (close) s_win_us = now;
    int w = 0;                                      /* forget the tasks that are gone */
    for (int i = 0; i < s_ntr; i++) if (s_tr[i].seen) s_tr[w++] = s_tr[i];
    s_ntr = w;
    return (int)n;
}

int aos_hal_tasks(aos_task_info_t *out, int max)
{
    if (!tasks_init()) return 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int n = refresh_locked();
    for (int k = 0; k < n && k < max && out; k++) {
        const TaskStatus_t *t = &s_st[k];
        aos_task_info_t *o = &out[k];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", t->pcTaskName ? t->pcTaskName : "?");
        o->id = (uint32_t)t->xTaskNumber;
        /* TaskStatus_t.xCoreID needs CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; IDF's own call does not */
        BaseType_t core = xTaskGetCoreID(t->xHandle);
        o->core = core == tskNO_AFFINITY || core < 0 || core > 1 ? -1 : (int8_t)core;
        o->state = state_of(t->eCurrentState);
        o->prio = (uint8_t)t->uxCurrentPriority;
        o->base_prio = (uint8_t)t->uxBasePriority;
        track_t *tr = track_find(t->xTaskNumber);
        o->stack_min_free = tr ? tr->mark : -1;
        o->stack_psram = esp_ptr_external_ram(t->pxStackBase);
        o->stack_size = -1;
        TaskSnapshot_t snap;
        if (vTaskGetSnapshot(t->xHandle, &snap) == pdTRUE && snap.pxEndOfStack > t->pxStackBase) {
            /* pxEndOfStack is the stack's last usable word, at the top */
            uint32_t sz = (uint32_t)((uint8_t *)snap.pxEndOfStack - (uint8_t *)t->pxStackBase) + 16;
            sz &= ~15u;
            if (sz >= 256 && sz <= 1024 * 1024) o->stack_size = (int32_t)sz;
        }
        o->cpu_x10 = tr ? tr->cpu_x10 : -1;
        o->run_us = tr ? tr->total : (uint64_t)t->ulRunTimeCounter;
    }
    xSemaphoreGive(s_mx);
    return n;
}

/* -------------------------------------------------------------------------- */
/* The Wi-Fi link, watched by the stats task                                   */
/* -------------------------------------------------------------------------- */

static int64_t  s_net_since_us;         /* 0 while down */
static uint32_t s_net_drops;
static int      s_net_channel;
static uint8_t  s_net_bssid[6];
static bool     s_net_bssid_ok;

/* The link to the C6. Twice on 2026-10-03, during an OTA upload, the Wi-Fi
 * died with the rest of the board alive: the screen and the touch went on,
 * the network and the portal did not, nothing in the log said why, and it
 * stayed so until RESET. Every Wi-Fi call goes to the C6 over SDIO, so the
 * call this task makes every 5 s anyway (the AP's record, or the mode when
 * there is no AP to ask about) is the link's heartbeat: an RPC that does
 * not come back times out in esp_hosted after 5 s, and LINK_DEAD of those
 * in a row mean the link is gone. One that never returns at all (esp_hosted
 * queues it towards the C6 with no time limit, and that queue fills when
 * the link stops) is caught by a timer of its own: a call out longer than
 * LINK_STUCK_US.
 *
 * esp_hosted has no way back from that short of a restart (its own driver
 * restarts the board on the SDIO errors it does see), so this writes the
 * tasks to the log and restarts too; the next boot resets the C6 through
 * its reset line, and the log survives (GET /api/log?prev=1). At most
 * LINK_TRIES times in a row (a counter in noinit PSRAM, cleared once the
 * link has answered for LINK_HEALTHY_S), so a C6 gone for good leaves the
 * board up without network rather than in a loop. */
#define LINK_DEAD       3
#define LINK_SLOW_US    (4 * 1000000LL)     /* a failure this slow is a timeout, not an answer */
#define LINK_STUCK_US   (20 * 1000000LL)
#define LINK_TRIES      3
#define LINK_HEALTHY_S  600
#define LINK_MAGIC      0x43364C4Bu         /* "C6LK" */
static EXT_RAM_NOINIT_ATTR struct { uint32_t magic, count; } s_link_boots;
static volatile int64_t s_link_call_us;     /* when the call out started; 0: none out */
static esp_timer_handle_t s_link_timer;

static void link_dead(const char *why)
{
    static volatile bool done;
    if (done) return;
    done = true;
    if (s_link_boots.magic != LINK_MAGIC) { s_link_boots.magic = LINK_MAGIC; s_link_boots.count = 0; }
    if (s_link_boots.count >= LINK_TRIES) {
        ESP_LOGE(TAG, "C6 link: dead (%s), and restarted %d times in a row already; staying up without network", why,
                 LINK_TRIES);
        return;
    }
    s_link_boots.count++;
    ESP_LOGE(TAG, "C6 link: dead (%s); restarting, try %u of %d", why, (unsigned)s_link_boots.count, LINK_TRIES);
    void aos_p4_log_tasks(void);
    aos_p4_log_tasks();
    vTaskDelay(pdMS_TO_TICKS(200));
    /* esp_wifi_stop runs at shutdown, and it is one more RPC to wait 5 s for
     * (esp_hosted's own restart drops it the same way) */
    esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
    esp_restart();
}

static void link_timer_cb(void *arg)
{
    (void)arg;
    int64_t at = s_link_call_us;
    bool aos_net_p4_c6_updating(void);
    if (at && esp_timer_get_time() - at > LINK_STUCK_US && !aos_net_p4_c6_updating()) link_dead("an RPC out for over 20 s");
}

static void link_call_start(void)
{
    if (!s_link_timer) {
        const esp_timer_create_args_t a = { .callback = link_timer_cb, .name = "c6-link" };
        if (esp_timer_create(&a, &s_link_timer) == ESP_OK) esp_timer_start_periodic(s_link_timer, 5 * 1000000LL);
    }
    s_link_call_us = esp_timer_get_time();
}

static void link_call_end(bool ok)
{
    static int dead;
    static int64_t alive_since;
    int64_t now = esp_timer_get_time(), took = now - s_link_call_us;
    s_link_call_us = 0;
    bool aos_net_p4_c6_updating(void);
    if (aos_net_p4_c6_updating()) { dead = 0; return; }    /* the C6 is busy taking its new firmware */
    if (s_link_boots.magic != LINK_MAGIC) { s_link_boots.magic = LINK_MAGIC; s_link_boots.count = 0; }
    if (ok || took < LINK_SLOW_US) {        /* an error that came back quickly is still the C6 talking */
        if (dead) ESP_LOGW(TAG, "C6 link: answering again after %d timeouts", dead);
        dead = 0;
        if (!alive_since) alive_since = now;
        if (s_link_boots.count && now - alive_since > LINK_HEALTHY_S * 1000000LL) s_link_boots.count = 0;
        return;
    }
    alive_since = 0;
    ESP_LOGW(TAG, "C6 link: no answer in %d ms (%d in a row)", (int)(took / 1000), dead + 1);
    if (++dead >= LINK_DEAD) link_dead("3 RPCs in a row timed out");
}

static void net_watch(int64_t now)
{
    static int64_t ap_at, mode_at;
    bool up = aos_hal_net_state() == AOS_NET_CONNECTED;
    if (up && !s_net_since_us) {
        s_net_since_us = now;
        ap_at = 0;
    } else if (!up && s_net_since_us) {
        s_net_since_us = 0;
        s_net_drops++;
        s_net_channel = 0;
        s_net_bssid_ok = false;
    }
    /* The AP's record is an RPC to the C6: once when the link comes up and
     * every 5 s after, here and nowhere else - the status bar and the
     * Monitor read what this keeps (aos_hal_net_rssi). */
    if (up && (!ap_at || now - ap_at > 5 * 1000000LL)) {
        ap_at = now;
        wifi_ap_record_t ap;
        link_call_start();
        bool ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        link_call_end(ok);
        if (ok) {
            s_net_channel = ap.primary;
            memcpy(s_net_bssid, ap.bssid, 6);
            s_net_bssid_ok = true;
            void aos_net_p4_note_rssi(int rssi);
            aos_net_p4_note_rssi(ap.rssi);
        }
    } else if (!up && (!mode_at || now - mode_at > 5 * 1000000LL)) {
        /* No AP to ask about (connecting, or only the access point up): the
         * mode is an RPC as well. Not before the radio came up. */
        mode_at = now;
        bool aos_net_p4_up(void);
        if (aos_net_p4_up()) {
            wifi_mode_t m;
            link_call_start();
            link_call_end(esp_wifi_get_mode(&m) == ESP_OK);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* System facts                                                                */
/* -------------------------------------------------------------------------- */

static const char *reset_key(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int-wdt";
    case ESP_RST_TASK_WDT:   return "task-wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deep-sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    default:                 return "unknown";
    }
}

static uint32_t s_app_bytes;            /* measured once, by the stats task */

/* It reads the image header from the flash, and the stats task's stack is
 * in PSRAM: through aos_flash_call (aos_flashop_p4.c). */
void aos_flash_call(void (*fn)(void *ctx), void *ctx);

static void app_measure_now(void *ctx)
{
    (void)ctx;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return;
    esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t md;
    if (esp_image_get_metadata(&pos, &md) == ESP_OK) s_app_bytes = md.image_len;
}

static void app_measure(void) { aos_flash_call(app_measure_now, NULL); }

static void mem_region(aos_mem_region_t *m, uint32_t caps)
{
    m->free = heap_caps_get_free_size(caps);
    m->total = heap_caps_get_total_size(caps);
    m->largest = heap_caps_get_largest_free_block(caps);
    m->min_free = heap_caps_get_minimum_free_size(caps);
}

static void ip4(char *out, size_t n, const esp_ip4_addr_t *a)
{
    if (a->addr) snprintf(out, n, IPSTR, IP2STR(a));
    else out[0] = 0;
}

bool aos_hal_sys_info(aos_sys_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    out->chip = "ESP32-P4";
    out->chip_rev = ci.revision;
    out->cores = ci.cores;
    out->cpu_mhz = esp_clk_cpu_freq() / 1000000;
    out->idf_version = esp_get_idf_version();
    out->reset_reason = reset_key(esp_reset_reason());
    uint32_t fs = 0;
    if (esp_flash_get_size(NULL, &fs) == ESP_OK) out->flash_bytes = fs;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p) {
        out->app_slot = p->label;
        out->app_slot_bytes = p->size;
    }
    out->app_bytes = s_app_bytes;

    mem_region(&out->mem[AOS_MEM_INTERNAL], MALLOC_CAP_INTERNAL);
    mem_region(&out->mem[AOS_MEM_DMA], MALLOC_CAP_DMA);
    mem_region(&out->mem[AOS_MEM_SPIRAM], MALLOC_CAP_SPIRAM);
    mem_region(&out->mem[AOS_MEM_EXEC], MALLOC_CAP_EXEC);

    int64_t since = s_net_since_us;
    out->net_up_s = since ? (uint32_t)((esp_timer_get_time() - since) / 1000000) : 0;
    out->net_drops = s_net_drops;
    out->net_channel = s_net_channel;
    if (s_net_bssid_ok) {
        const uint8_t *b = s_net_bssid;
        snprintf(out->net_bssid, sizeof out->net_bssid, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    }
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif) {
        uint8_t mac[6];
        if (esp_netif_get_mac(nif, mac) == ESP_OK)
            snprintf(out->net_mac, sizeof out->net_mac, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
            ip4(out->net_gw, sizeof out->net_gw, &ip.gw);
            ip4(out->net_mask, sizeof out->net_mask, &ip.netmask);
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
            ip4(out->net_dns, sizeof out->net_dns, &dns.ip.u_addr.ip4);
    }
    /* lwIP counts no bytes without CONFIG_LWIP_STATS, which this build keeps off */
    out->net_rx_bytes = out->net_tx_bytes = -1;
    out->note = NULL;
    return true;
}

/* -------------------------------------------------------------------------- */
/* The stats task                                                              */
/* -------------------------------------------------------------------------- */

static void stats_task(void *arg)
{
    (void)arg;
    app_measure();
    int64_t tasks_at = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        aos_stats_tick();               /* does its work once a second */
        net_watch(now);
        /* Every ten seconds the task list moves its totals forward, so a
         * wrap of a 32-bit counter (71 min) is never missed, and the CPU
         * window stays short even with nobody looking. */
        if (now - tasks_at >= 10 * 1000000LL && tasks_init()) {
            tasks_at = now;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            refresh_locked();
            xSemaphoreGive(s_mx);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void aos_tasks_p4_start(void)
{
    tasks_init();
    /* Low priority on purpose: under load the samples arrive late, which is
     * better than the sampler being what loads the CPU. The stack also
     * covers aos_stats.c's temperature sensor driver. */
    if (xTaskCreatePinnedToCoreWithCaps(stats_task, "stats", 4096, NULL, 1, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "could not start the stats task");
}
