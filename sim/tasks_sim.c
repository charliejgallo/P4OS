/*
 * P4OS simulator - what the Monitor shows (aos.sysmon, /api/sysmon), on the
 * Mac. The board's version is components/aos_hal/aos_tasks_p4.c.
 *
 * What is REAL here:
 *   - the task list is the simulator's own threads (Mach task_threads):
 *     their names, state, macOS priority, CPU per refresh and run time;
 *   - the stack size of each thread and its "least free", which is the
 *     lowest the stack pointer has been seen at when sampled (once a second
 *     and on every refresh), not a true high-water mark;
 *   - the CPU load: core 1 is the main thread (where LVGL runs, as it is
 *     pinned to core 1 on the board) and core 0 is all the other threads
 *     together, capped at 100 %;
 *   - "PSRAM" in use is the process's malloc heap, out of the board's 32 MB.
 *
 * What is MADE UP, and said so in aos_sys_info_t.note: the internal RAM,
 * the chip's temperature (it follows the load), the network's details
 * beyond hal_sim.c's and the card's (consistent with hal_sim.c's 32 GB).
 *
 * The hour of history takes one sample a minute like the board;
 * P4_SIM_STATS_MINUTE_MS=1000 makes a "minute" one second, to see the
 * charts fill without waiting an hour.
 */
#include "aos_hal.h"

#include <mach/mach.h>
#include <malloc/malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PSRAM_TOTAL     (32u * 1024 * 1024)
#define INT_TOTAL       (492u * 1024)       /* about what the P4 leaves the heap with 256 KB of L2 cache */

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static thread_t s_main_port;
static int64_t  s_start_us;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* -------------------------------------------------------------------------- */
/* Threads                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t tid;
    uint64_t run_us, win_run_us;
    int16_t  cpu_x10;
    int32_t  min_free;          /* least free seen, -1 none */
    bool     seen;
} track_t;

static track_t s_tr[AOS_TASKS_MAX];
static int     s_ntr;
static int64_t s_win_us;

static track_t *track_find(uint64_t tid)
{
    for (int i = 0; i < s_ntr; i++) if (s_tr[i].tid == tid) return &s_tr[i];
    return NULL;
}

static uint64_t thread_sp(thread_t t)
{
#if defined(__arm64__)
    arm_thread_state64_t st;
    mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS) return 0;
    return (uint64_t)arm_thread_state64_get_sp(st);
#elif defined(__x86_64__)
    x86_thread_state64_t st;
    mach_msg_type_number_t c = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(t, x86_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS) return 0;
    return st.__rsp;
#else
    (void)t;
    return 0;
#endif
}

static uint8_t state_of(int s)
{
    switch (s) {
    case TH_STATE_RUNNING:         return AOS_TASK_RUNNING;
    case TH_STATE_WAITING:         return AOS_TASK_BLOCKED;
    case TH_STATE_UNINTERRUPTIBLE: return AOS_TASK_BLOCKED;
    case TH_STATE_STOPPED:         return AOS_TASK_SUSPENDED;
    case TH_STATE_HALTED:          return AOS_TASK_DELETED;
    default:                       return AOS_TASK_UNKNOWN;
    }
}

/* Walks the threads, moves the counters forward and, with the window half
 * a second old or more, closes it. out may be NULL (the sampler). With s_mx
 * held. Returns the number of threads; *main_x10 / *rest_x10 get the CPU of
 * the main thread and of the others over the window just closed. */
static int walk_locked(aos_task_info_t *out, int max, int *main_x10, int *rest_x10)
{
    thread_act_array_t th;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return 0;
    int64_t now = now_us();
    int64_t span = now - s_win_us;
    bool close = !s_win_us || span >= 500000;
    int m_x10 = 0, r_x10 = 0;

    for (int i = 0; i < s_ntr; i++) s_tr[i].seen = false;
    for (mach_msg_type_number_t k = 0; k < n; k++) {
        thread_extended_info_data_t ei;
        mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
        thread_identifier_info_data_t ii;
        mach_msg_type_number_t ci = THREAD_IDENTIFIER_INFO_COUNT;
        bool ok = thread_info(th[k], THREAD_EXTENDED_INFO, (thread_info_t)&ei, &c) == KERN_SUCCESS &&
                  thread_info(th[k], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ci) == KERN_SUCCESS;
        if (ok) {
            uint64_t run = (ei.pth_user_time + ei.pth_system_time) / 1000;     /* ns -> us */
            track_t *tr = track_find(ii.thread_id);
            if (!tr && s_ntr < AOS_TASKS_MAX) {
                tr = &s_tr[s_ntr++];
                *tr = (track_t){ .tid = ii.thread_id, .run_us = run, .win_run_us = run, .cpu_x10 = -1, .min_free = -1 };
            }
            bool is_main = th[k] == s_main_port;
            int32_t size = -1;
            pthread_t pt = pthread_from_mach_thread_np(th[k]);
            if (pt) {
                size = (int32_t)pthread_get_stacksize_np(pt);
                uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pt), sp = thread_sp(th[k]);
                if (tr && sp && sp <= top && top - sp <= (uint64_t)size) {
                    int32_t fr = size - (int32_t)(top - sp);
                    if (tr->min_free < 0 || fr < tr->min_free) tr->min_free = fr;
                }
            }
            if (tr) {
                tr->seen = true;
                tr->run_us = run;
                if (close) {
                    if (s_win_us && span > 0) {
                        int64_t x = (int64_t)(run - tr->win_run_us) * 1000 / span;
                        tr->cpu_x10 = (int16_t)(x < 0 ? 0 : x > 1000 ? 1000 : x);
                        if (is_main) m_x10 += tr->cpu_x10;
                        else r_x10 += tr->cpu_x10;
                    }
                    tr->win_run_us = run;
                }
            }
            if (out && (int)k < max) {
                aos_task_info_t *o = &out[k];
                memset(o, 0, sizeof *o);
                const char *nm = ei.pth_name;
                if (!strncmp(nm, "com.apple.", 10)) nm += 10;
                if (is_main) snprintf(o->name, sizeof o->name, "main (LVGL)");
                else if (nm[0]) snprintf(o->name, sizeof o->name, "%s", nm);
                else snprintf(o->name, sizeof o->name, "hilo %u", (unsigned)k);
                o->id = (uint32_t)ii.thread_id;
                o->core = is_main ? 1 : -1;
                o->state = state_of(ei.pth_run_state);
                o->prio = (uint8_t)ei.pth_curpri;
                o->base_prio = (uint8_t)ei.pth_priority;
                o->stack_size = size;
                o->stack_min_free = tr ? tr->min_free : -1;
                o->stack_psram = false;
                o->cpu_x10 = tr ? tr->cpu_x10 : -1;
                o->run_us = run;
            }
        }
        mach_port_deallocate(mach_task_self(), th[k]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
    if (close) s_win_us = now;
    int w = 0;
    for (int i = 0; i < s_ntr; i++) if (s_tr[i].seen) s_tr[w++] = s_tr[i];
    s_ntr = w;
    if (main_x10) *main_x10 = close ? m_x10 : -1;
    if (rest_x10) *rest_x10 = close ? r_x10 : -1;
    return (int)n;
}

int aos_hal_tasks(aos_task_info_t *out, int max)
{
    pthread_mutex_lock(&s_mx);
    int n = walk_locked(out, max, NULL, NULL);
    pthread_mutex_unlock(&s_mx);
    return n;
}

/* -------------------------------------------------------------------------- */
/* The sampler: live values once a second, a minute ring                       */
/* -------------------------------------------------------------------------- */

static aos_sys_stats_t s_now = { .chip_c = NAN, .pmu_c = NAN, .board_c = NAN, .cpu_load = { -1, -1 } };
static int16_t s_min[AOS_HIST_COUNT][AOS_MIN_HIST_LEN];
static int     s_min_head;
static bool    s_have_stats;
static int     s_cpu_acc[2], s_cpu_n;
static uint32_t s_min_free[AOS_MEM_COUNT];

static void mem_sample(void)
{
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    uint32_t used = st.size_in_use > PSRAM_TOTAL - 512 * 1024 ? PSRAM_TOTAL - 512 * 1024 : (uint32_t)st.size_in_use;
    s_now.psram_total = PSRAM_TOTAL;
    s_now.psram_free = PSRAM_TOTAL - used;
    /* the internal RAM is invented: a plausible level that breathes a little */
    double t = (double)(now_us() - s_start_us) / 1e6;
    uint32_t wobble = (uint32_t)(6144 + 4096 * sin(t / 23.0) + 1024 * sin(t / 3.1));
    s_now.int_total = INT_TOTAL;
    s_now.int_free = 214u * 1024 - wobble;
    s_now.int_largest = 104u * 1024 - wobble / 4;
    s_now.exec_total = s_now.int_total;
    s_now.exec_free = s_now.int_free;
    s_now.exec_largest = s_now.int_largest;
}

static void minute_push(void)
{
    int h = s_min_head;
    s_min[AOS_HIST_CHIP_T][h] = isnan(s_now.chip_c) ? AOS_HIST_NONE : (int16_t)lrintf(s_now.chip_c * 10);
    s_min[AOS_HIST_PMU_T][h] = AOS_HIST_NONE;
    s_min[AOS_HIST_BOARD_T][h] = AOS_HIST_NONE;
    s_min[AOS_HIST_INT_FREE_KB][h] = (int16_t)(s_now.int_free / 1024);
    s_min[AOS_HIST_PSRAM_FREE_KB][h] = (int16_t)(s_now.psram_free / 1024 > 32767 ? 32767 : s_now.psram_free / 1024);
    for (int c = 0; c < 2; c++) {
        s_min[AOS_HIST_CPU0 + c][h] = s_cpu_n ? (int16_t)(s_cpu_acc[c] / s_cpu_n) : AOS_HIST_NONE;
        s_cpu_acc[c] = 0;
    }
    s_cpu_n = 0;
    s_min_head = (h + 1) % AOS_MIN_HIST_LEN;
}

static void *sampler(void *arg)
{
    (void)arg;
    pthread_setname_np("stats");
    const char *e = getenv("P4_SIM_STATS_MINUTE_MS");
    int64_t minute_us = (e && atoi(e) > 0 ? atoi(e) : 60000) * 1000LL;
    int64_t min_at = now_us();
    for (;;) {
        usleep(1000000);
        pthread_mutex_lock(&s_mx);
        int m = -1, r = -1;
        walk_locked(NULL, 0, &m, &r);
        if (m >= 0) {
            int load1 = (m + 5) / 10, load0 = (r + 5) / 10;
            s_now.cpu_load[0] = load0 > 100 ? 100 : load0;
            s_now.cpu_load[1] = load1 > 100 ? 100 : load1;
            s_cpu_acc[0] += s_now.cpu_load[0];
            s_cpu_acc[1] += s_now.cpu_load[1];
            s_cpu_n++;
        }
        mem_sample();
        /* a chip that warms with the load, slowly */
        float target = 39.5f + 0.09f * (float)(s_now.cpu_load[0] + s_now.cpu_load[1]);
        s_now.chip_c = isnan(s_now.chip_c) ? target : s_now.chip_c + (target - s_now.chip_c) * 0.08f;
        uint32_t fr[AOS_MEM_COUNT] = { s_now.int_free, s_now.int_free, s_now.psram_free, s_now.exec_free };
        for (int k = 0; k < AOS_MEM_COUNT; k++) if (!s_min_free[k] || fr[k] < s_min_free[k]) s_min_free[k] = fr[k];
        s_have_stats = true;
        int64_t now = now_us();
        if (now - min_at >= minute_us) {
            min_at = now;
            minute_push();
        }
        pthread_mutex_unlock(&s_mx);
    }
    return NULL;
}

__attribute__((constructor)) static void tasks_sim_start(void)
{
    s_main_port = pthread_mach_thread_np(pthread_self());   /* constructors run on the main thread */
    s_start_us = now_us();
    for (int k = 0; k < AOS_HIST_COUNT; k++)
        for (int i = 0; i < AOS_MIN_HIST_LEN; i++) s_min[k][i] = AOS_HIST_NONE;
    pthread_t t;
    if (pthread_create(&t, NULL, sampler, NULL) == 0) pthread_detach(t);
}

bool aos_hal_sys_stats(aos_sys_stats_t *out)
{
    if (!out) return false;
    pthread_mutex_lock(&s_mx);
    *out = s_now;
    bool ok = s_have_stats;
    pthread_mutex_unlock(&s_mx);
    return ok;
}

int aos_hal_minute_history(aos_hist_t which, int16_t *out, int max)
{
    if (which < 0 || which >= AOS_HIST_COUNT || !out) return 0;
    int n = max < AOS_MIN_HIST_LEN ? max : AOS_MIN_HIST_LEN;
    pthread_mutex_lock(&s_mx);
    int start = (s_min_head + AOS_MIN_HIST_LEN - n) % AOS_MIN_HIST_LEN;
    for (int i = 0; i < n; i++) out[i] = s_min[which][(start + i) % AOS_MIN_HIST_LEN];
    pthread_mutex_unlock(&s_mx);
    return n;
}

/* -------------------------------------------------------------------------- */
/* System facts                                                                */
/* -------------------------------------------------------------------------- */

bool aos_hal_sys_info(aos_sys_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->chip = "ESP32-P4";
    out->chip_rev = 301;
    out->cores = 2;
    out->cpu_mhz = 400;
    out->idf_version = "simulador";
    out->reset_reason = "power-on";
    out->flash_bytes = 32u * 1024 * 1024;
    out->app_slot = "ota_0";
    out->app_slot_bytes = 8u * 1024 * 1024;
    out->app_bytes = 0;                         /* the Mac's executable says nothing about the board's */
    pthread_mutex_lock(&s_mx);
    aos_sys_stats_t s = s_now;
    uint32_t mn[AOS_MEM_COUNT];
    memcpy(mn, s_min_free, sizeof mn);
    pthread_mutex_unlock(&s_mx);
    out->mem[AOS_MEM_INTERNAL] = (aos_mem_region_t){ s.int_free, s.int_total, s.int_largest, mn[AOS_MEM_INTERNAL] };
    out->mem[AOS_MEM_DMA] = (aos_mem_region_t){ s.int_free - 12288, s.int_total - 16384, s.int_largest, mn[AOS_MEM_DMA] - 12288 };
    out->mem[AOS_MEM_SPIRAM] = (aos_mem_region_t){ s.psram_free, s.psram_total, s.psram_free - 3u * 1024 * 1024, mn[AOS_MEM_SPIRAM] };
    out->mem[AOS_MEM_EXEC] = (aos_mem_region_t){ s.exec_free, s.exec_total, s.exec_largest, mn[AOS_MEM_EXEC] };
    bool up = aos_hal_net_state() == AOS_NET_CONNECTED;
    out->net_up_s = up ? (uint32_t)((now_us() - s_start_us) / 1000000) : 0;
    out->net_drops = 0;
    out->net_channel = up ? 6 : 0;
    snprintf(out->net_bssid, sizeof out->net_bssid, "%s", up ? "02:00:00:C6:00:01" : "");
    snprintf(out->net_mac, sizeof out->net_mac, "02:50:34:53:49:4D");      /* "P4SIM" */
    snprintf(out->net_mask, sizeof out->net_mask, "255.0.0.0");
    out->net_rx_bytes = out->net_tx_bytes = -1;
    out->note = "Simulador: las tareas son los hilos del proceso; el núcleo 1 es el hilo principal "
                "(LVGL) y el 0, todos los demás. La RAM interna, la temperatura, la red y la "
                "tarjeta son inventadas; la pila libre es el mínimo que se vio al muestrear.";
    return true;
}

bool aos_hal_sd_info(aos_sd_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "SIMFS");
    out->kind = "SDHC";
    out->fs = "FAT32";
    out->capacity = 32ULL * 1024 * 1024 * 1024;     /* what hal_sim.c's aos_hal_sd_usage says */
    out->freq_khz = 40000;
    out->bus_width = 4;
    out->manufacturer = 3;
    out->year = 2024;
    out->month = 5;
    return true;
}
