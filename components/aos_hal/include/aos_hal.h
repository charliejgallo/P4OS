/*
 * P4OS - Hardware Abstraction Layer (inherited from AmoledOS)
 *
 * The single contract between the UI/apps and the platform. There are two
 * implementations:
 *   - aos_hal_p4.c : Waveshare ESP32-P4-WIFI6-Touch-LCD-5
 *   - hal_sim.c    : LVGL/SDL simulator on the Mac
 *
 * The contract keeps AmoledOS's names so that apps port with small diffs.
 * Whatever the P4 board lacks (IMU, PMU, RTC chip) is still declared here and
 * answers "not available"; aos_hal_caps() says up front what there is.
 *
 * Rule: neither aos_ui nor aos_apps includes an ESP-IDF header. Only LVGL and
 * this one.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Display                                                                     */
/* -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Large static variables: send them to PSRAM
 *
 * On this board the scarce resource is internal RAM, not PSRAM (there are
 * 8 MB and 8 to spare). And running short of internal RAM does not raise an
 * error: it puts garbage on the screen, because allocating the flush's
 * intermediate DMA buffer fails (see docs/HANDOFF-APPS.md). So any fat static
 * structure that does not need to be inside is better off outside.
 *
 * Marks a .bss variable to live in PSRAM. Requires
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY, which is enabled.
 *
 * Do NOT use it for:
 *   - anything an ISR or DMA touches,
 *   - anything read before PSRAM is initialised,
 *   - small or very hot things: PSRAM goes through the cache and is slower.
 *
 * In the simulator and in dynamic apps it evaporates. */
#if defined(AOS_SIM)
#define AOS_BSS_PSRAM
#else
#include "esp_attr.h"
#define AOS_BSS_PSRAM       EXT_RAM_BSS_ATTR
#endif

/* --------------------------------------------------------------------------
 * Screen size and orientation
 *
 * The panel is 720x1280, portrait. The system can run in portrait or
 * landscape (the person picks; there is no IMU), so the size an app sees is
 * the LOGICAL one and changes at run time: AOS_SCREEN_W/H are function calls,
 * not constants. An app that sized a static array with them does not compile
 * any more, on purpose - that is exactly the code that needs porting.
 *
 * AOS_PANEL_W/H are the native size, for the rare code (frame buffers,
 * the retro canvas) that needs it.
 * -------------------------------------------------------------------------- */
#define AOS_PANEL_W         720
#define AOS_PANEL_H         1280

int  aos_hal_screen_w(void);        /* logical width, 720 or 1280  */
int  aos_hal_screen_h(void);        /* logical height, 1280 or 720 */
#define AOS_SCREEN_W        aos_hal_screen_w()
#define AOS_SCREEN_H        aos_hal_screen_h()
#define AOS_SCREEN_RADIUS   0       /* square glass */

/* 0 = portrait (native), 90 / 270 = landscape, 180 = upside down. Takes
 * effect immediately: the HAL rotates the output and the touch, the caller
 * (aos_ui) re-lays out whatever is on screen. */
void aos_hal_display_set_rotation(int degrees);
int  aos_hal_display_get_rotation(void);

/* What LVGL has sent to the panel since boot: pixels and frames. The Monitor
 * turns it into a redraw rate, which is what tells a screen that keeps
 * repainting itself for nothing. false where it is not counted. */
bool aos_hal_display_flush_count(uint64_t *px, uint32_t *frames);
/* P4OS: frames the apps put on the panel whole with aos_hal_display_flip(),
 * since boot (LVGL's own are counted by its render event). */
uint32_t aos_hal_display_flips(void);

/* P4OS: how fast LVGL redraws the whole screen, `frames` times in a row
 * (up to 200), with the draw buffers as the boot left them (pref "lvbuf":
 * 0 two in internal RAM, 1 one, 2 two in PSRAM, 3 one; "lvrows"). */
typedef struct {
    int      frames, flushes, mode, rows, buffers;
    uint32_t us_per_frame;
} aos_display_bench_t;
bool aos_hal_display_bench(int frames, aos_display_bench_t *out);

/* P4OS: the screen as the person sees it (upright, the apps' own drawing
 * included) to /sdcard/photos/Capturas/P4OS_<date>.bmp, an album of
 * Photos; the path, from the card's
 * root, in path_out. false without a card. Takes a few hundred ms: not from
 * the LVGL task. */
bool aos_hal_display_save(char *path_out, size_t n);

/* P4OS: the tuning preferences (/api/tune) back to their defaults: the safe
 * boot and the BOOT button's safe mode. Read at boot. */
void aos_hal_tune_reset(void);

/* P4OS: the hang watchdog (aos_hal_p4.c): main.c marks the boot's stages
 * and beats from its main loop; when the boot has not finished 60 s in or
 * the beat stops for 30 s, the board writes where it was and restarts by
 * software, which keeps the log (GET /api/log?prev=1). Twice in a row at
 * most. */
void aos_hal_boot_stage(const char *stage);
void aos_hal_alive(void);
static inline bool aos_hal_landscape(void) { return aos_hal_screen_w() > aos_hal_screen_h(); }

/* Kept for apps written for the watch: the whole screen is comfortable. */
#define AOS_TOUCH_Y_MIN        0
#define AOS_TOUCH_Y_MAX        (AOS_SCREEN_H - 1)
#define AOS_TOUCH_LAND_TOP     0
#define AOS_TOUCH_LAND_BOTTOM  (AOS_SCREEN_H - 1)
#define AOS_TOUCH_LAND_LEFT    0
#define AOS_TOUCH_LAND_RIGHT   (AOS_SCREEN_W - 1)

/* --------------------------------------------------------------------------
 * Capabilities
 *
 * What this board (or the simulator) has. An app asks instead of assuming:
 * the same .so runs where there is an IMU and where there is not.
 * -------------------------------------------------------------------------- */
typedef enum {
    AOS_CAP_IMU         = 1u << 0,
    AOS_CAP_BATTERY     = 1u << 1,  /* a battery is connected and measured */
    AOS_CAP_PMU         = 1u << 2,  /* current, rails, charge control      */
    AOS_CAP_RTC         = 1u << 3,  /* time survives power loss            */
    AOS_CAP_MIC         = 1u << 4,
    AOS_CAP_SPEAKER     = 1u << 5,
    AOS_CAP_DUPLEX      = 1u << 6,  /* record and play at the same time    */
    AOS_CAP_WIFI        = 1u << 7,
    AOS_CAP_BLE         = 1u << 8,
    AOS_CAP_ESPNOW      = 1u << 9,  /* the aos_hal_link_* radio link       */
    AOS_CAP_USB_DEVICE  = 1u << 10,
    AOS_CAP_USB_HOST    = 1u << 11,
    AOS_CAP_CAMERA      = 1u << 12,
    AOS_CAP_HEADER      = 1u << 13, /* the 40-pin expansion header         */
    AOS_CAP_MULTITOUCH  = 1u << 14,
    AOS_CAP_ROTATION    = 1u << 15,
} aos_cap_t;

uint32_t aos_hal_caps(void);
static inline bool aos_hal_has(aos_cap_t cap) { return (aos_hal_caps() & cap) != 0; }

/* -------------------------------------------------------------------------- */
/* Battery / PMU (AXP2101)                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    int   percent;      /* 0..100, -1 if unknown      */
    float voltage;      /* V   */
    float current;      /* mA, positive = charging    */
    float temperature;  /* degrees C, NAN if n/a      */
    bool  charging;
    bool  usb_present;
} aos_battery_t;

bool aos_hal_battery_read(aos_battery_t *out);

/* -------------------------------------------------------------------------- */
/* IMU (QMI8658)                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    float ax, ay, az;   /* g     */
    float gx, gy, gz;   /* dps   */
    float temperature;  /* degrees C */
} aos_imu_t;

typedef enum {
    AOS_ORIENT_UP = 0,      /* screen facing the user, upright */
    AOS_ORIENT_DOWN,
    AOS_ORIENT_LEFT,
    AOS_ORIENT_RIGHT,
    AOS_ORIENT_FACE_UP,
    AOS_ORIENT_FACE_DOWN,
} aos_orientation_t;

bool              aos_hal_imu_read(aos_imu_t *out);
aos_orientation_t aos_hal_imu_orientation(void);
uint32_t          aos_hal_imu_steps(void);
void              aos_hal_imu_steps_reset(void);

/* -------------------------------------------------------------------------- */
/* Time (PCF85063 RTC + system time)                                           */
/* -------------------------------------------------------------------------- */

void aos_hal_time_now(struct tm *out);
bool aos_hal_time_set(const struct tm *t);          /* writes system + RTC     */
bool aos_hal_time_is_valid(void);                   /* false = never been set  */
void aos_hal_timezone_set(const char *tz);          /* POSIX TZ format         */
const char *aos_hal_timezone_get(void);

/* Hardware alarm of the RTC: wakes the board from deep sleep. */
bool aos_hal_rtc_alarm_set(const struct tm *when);
void aos_hal_rtc_alarm_clear(void);

/* -------------------------------------------------------------------------- */
/* Brightness and power                                                        */
/* -------------------------------------------------------------------------- */

int  aos_hal_brightness_get(void);          /* 0..100 */
void aos_hal_brightness_set(int percent);   /* command 0x51 of the AMOLED panel */

/* --------------------------------------------------------------------------
 * Gestures from the touch controller itself
 *
 * The v2's CST820 detects swipes on its own and publishes them in a register.
 * During a fast swipe it stops sending intermediate coordinates, so LVGL never
 * gathers the 50 px it needs to declare a gesture: the chip's own has to be
 * used.
 *
 * Returns the pending gesture and consumes it. Always 0 in the simulator,
 * because there LVGL detects gestures properly.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_TOUCH_GESTURE_NONE = 0,
    AOS_TOUCH_GESTURE_DOWN,
    AOS_TOUCH_GESTURE_UP,
    AOS_TOUCH_GESTURE_LEFT,
    AOS_TOUCH_GESTURE_RIGHT,
} aos_touch_gesture_t;

aos_touch_gesture_t aos_hal_touch_gesture(void);

/* --------------------------------------------------------------------------
 * Two fingers (docs/GESTURES.md)
 *
 * The v2's CST820 reports a second finger in registers its datasheet never
 * documents; the HAL reads both on every touch read and publishes them here.
 * LVGL keeps seeing a single pointer, as always.
 *
 * Apps do not want this: they want aos_gesture.h, which turns these frames
 * into taps, drags and pinches and filters what the chip gets wrong. This is
 * the raw material, for the recogniser and for diagnostics.
 *
 * P4OS: coordinates are SCREEN pixels in the current orientation - the
 * board's HAL turns the GT911's points like it turns LVGL's pointer, and
 * aos_ui_touch_map() is the identity (it stays for ported code). In the
 * simulator the pointer is one finger; with Option held a second one mirrors
 * it around the centre, and scripts have "pinch".
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  count;         /* fingers in this sample: 0, 1 or 2           */
    int16_t  x[2], y[2];    /* [0] first; only the first 'count' are valid  */
    uint32_t seq;           /* +1 per NEW sample from the chip (~73 Hz)     */
    uint32_t t_ms;          /* uptime when that sample was read             */
} aos_touch_frame_t;

/* The latest sample. false = there is no touch panel. */
bool     aos_hal_touch_frame(aos_touch_frame_t *out);
/* Every sample newer than 'after_seq', oldest first, up to 'max' (the HAL
 * keeps the last 16). Returns how many. A consumer that keeps the seq of the
 * last one it saw gets them all, however slowly it runs. */
uint32_t aos_hal_touch_frames(uint32_t after_seq, aos_touch_frame_t *out,
                              uint32_t max);
/* Counters since boot: reads of the chip, and new samples among them. */
void     aos_hal_touch_stats(uint32_t *reads, uint32_t *samples);
/* true = the hardware can report a second finger. */
bool     aos_hal_touch_multi(void);

/* The CST820's registers 0x00..0x0E as of the last read, for the raw view
 * (Settings -> Touch -> Raw view). Returns the sample counter, 0 = not a
 * CST820 (the v1 board, the simulator). */
#define AOS_TOUCH_REGS 15
uint32_t aos_hal_touch_regs(uint8_t regs[AOS_TOUCH_REGS]);

/* One configuration register of the CST820 (0xEC..0xFE: scan period,
 * interrupt control, auto sleep, long-press reset...). DIAGNOSTICS: the chip
 * forgets them on its own resets, and a wrong value can leave the touch deaf
 * until a restart. false = not a CST820, or the bus said no. */
bool aos_hal_touch_reg_read(uint8_t reg, uint8_t *val);
bool aos_hal_touch_reg_write(uint8_t reg, uint8_t val);

/* --------------------------------------------------------------------------
 * Display state
 *
 * ACTIVE  normal brightness, the UI responds
 * AOD     dimmed always-on: the watchface is drawn in its minimal mode and the
 *         panel goes to its lowest brightness. On AMOLED black is a pixel
 *         switched off, so an almost entirely black face draws very little.
 * OFF     panel off
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_DISPLAY_ACTIVE = 0,
    AOS_DISPLAY_AOD,
    AOS_DISPLAY_OFF,
} aos_display_state_t;

aos_display_state_t aos_hal_display_state(void);
void aos_hal_display_set_state(aos_display_state_t state);

/* The UI is notified whenever the state changes, so the watchface can switch
 * to its dimmed variant. Runs outside the LVGL task: take aos_hal_lock(). */
void aos_hal_set_display_state_cb(void (*cb)(aos_display_state_t state));

/* The main loop's pause between ticks: 200 ms with the screen lit, a second
 * dimmed or off, returning at once when the display changes state. */
void aos_hal_main_wait(void);

/* Always-on: when off, ACTIVE goes straight to OFF. */
void aos_hal_aod_enable(bool enable);
bool aos_hal_aod_enabled(void);

/* Brightness used in AOD, as a percentage of the panel. */
void aos_hal_aod_brightness_set(int percent);
int  aos_hal_aod_brightness_get(void);

/* How long the screen waits, in seconds. Preferences, set from Settings and
 * the portal.
 *
 * active: from the last touch to leaving ACTIVE (dimmed with always-on, off
 *   without). 0 = the default it always had: 60 s with always-on, 30 without.
 * aod: from dimming to switching off. 0 = never; always-on still gives way
 *   under 15 % battery. Default 300. */
void     aos_hal_screen_timeouts_set(uint32_t active_s, uint32_t aod_s);
void     aos_hal_screen_timeouts_get(uint32_t *active_s, uint32_t *aod_s);

/* Raising the wrist lights the screen (the IMU's gesture). A preference, on
 * by default; off, only a touch or the button wake it. */
void     aos_hal_raise_wake_enable(bool on);
bool     aos_hal_raise_wake_enabled(void);

void aos_hal_display_on(bool on);           /* shortcut: ACTIVE / OFF */
bool aos_hal_display_is_on(void);

void aos_hal_activity(void);                /* kicks the auto-off timer */
/* P4OS: the auto-off policy, from LVGL's thread a few times a second
 * (aos_ui_tick). keep_awake counts as activity: the app in front asked for
 * the screen to stay on (AOS_APP_FLAG_KEEP_AWAKE). The timeout is
 * aos_hal_screen_timeouts_set()'s active_s; 0 never switches off. */
void aos_hal_screen_idle_tick(bool keep_awake);
void aos_hal_sleep(void);                   /* light sleep, wakes on touch/IMU */
void aos_hal_shutdown(void);                /* cut by the PMU */
void aos_hal_reboot(void);

/* -------------------------------------------------------------------------- */
/* Physical buttons                                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    AOS_BUTTON_BOOT = 0,    /* the board's BOOT button (GPIO0 on the watch, GPIO35 on the P4) */
    AOS_BUTTON_PWR,         /* the power button, through the PMU (the P4's reaches no GPIO) */
} aos_button_t;

/* What happened to the button. Both press and release are reported, not just
 * release, because a game needs to know how long it is held down (sustained
 * fire) and not merely that there was a click. */
typedef enum {
    AOS_BUTTON_PRESS = 0,   /* just pressed                                */
    AOS_BUTTON_CLICK,       /* released before the long-press threshold    */
    AOS_BUTTON_LONG,        /* released after the threshold                */
} aos_button_action_t;

/* The callback runs outside the LVGL task: if it touches the UI it has to take
 * aos_hal_lock() first. In the simulator the space bar fires it. */
void aos_hal_set_button_cb(void (*cb)(aos_button_t button, aos_button_action_t action));

/* P4OS: whether the button is held right now (the boot reads it for the
 * safe mode). */
bool aos_hal_button_is_down(aos_button_t button);

/* -------------------------------------------------------------------------- */
/* Power: what the PMU knows and what the firmware keeps about the battery     */
/* -------------------------------------------------------------------------- */

/* The charger's stage, straight from the AXP2101. */
typedef enum {
    AOS_CHG_TRICKLE = 0,    /* very flat cell, tiny current       */
    AOS_CHG_PRECHARGE,      /* below 3 V, precharge current       */
    AOS_CHG_CC,             /* constant current                   */
    AOS_CHG_CV,             /* constant voltage, current tapering */
    AOS_CHG_DONE,           /* terminated                         */
    AOS_CHG_IDLE,           /* not charging                       */
} aos_charge_state_t;

/* Everything aos_battery_t does not carry. aos_battery_t is frozen: the .so
 * apps were compiled against its size, so the extra detail lives here. */
typedef struct {
    aos_charge_state_t charge_state;
    float    vbus, vsys;            /* V                                        */
    float    board_temperature;     /* NTC next to the PMU, NAN if there is none */
    bool     battery_present;
    /* the charger's programme */
    int      charge_ma;             /* constant-current limit                   */
    int      charge_target_mv;      /* 4100 with battery care, 4200 without     */
    int      warn_pct, shutdown_pct;
    int      poweroff_mv;           /* where the PMU cuts everything (VOFF)      */
    /* kept by the firmware */
    float    drain_pct_per_hour;    /* NAN until it has seen enough             */
    float    hours_left;            /* NAN until it has seen enough             */
    uint32_t on_battery_s;          /* since USB was unplugged, 0 while plugged */
    uint32_t battery_minutes_total; /* lifetime on battery, survives restarts   */
    uint32_t charge_cycles;         /* completed charges, survives restarts     */
    const char *power_on_reason;    /* why the PMU came up this time            */
    const char *power_off_reason;   /* why it went down the last time           */
    int      cpu_mhz;               /* what the CPU is running at right now     */
    bool     panel_asleep;          /* the AMOLED's driver IC is in sleep-in    */
    bool     power_saving_active;   /* by preference or because the battery is low */
    bool     light_sleep;           /* automatic light sleep is armed right now */
    /* The state of charge is the firmware's own estimate (aos_soc.c); these
     * are the pieces it is built from. */
    int      gauge_pct;             /* the AXP2101's gauge, which reads high   */
    float    soc_sag_mv;            /* voltage lost with the screen lit        */
    int      soc_sag_samples;
    float    capacity_mah;          /* usable, up to the charge target         */
    int      capacity_samples;      /* 0 = still the default, never measured   */
} aos_power_info_t;

bool aos_hal_power_info(aos_power_info_t *out);

/* --------------------------------------------------------------------------
 * System statistics and their history (aos_stats.c, v0.5.1)
 *
 * For Settings' Battery and Diagnostics pages. The HAL samples on its own,
 * from the housekeeping task: the live values every second, a minute
 * history for the last hour, and the battery every five minutes for the
 * last 24 h, kept on the card so a restart does not wipe the graph.
 *
 * P4OS: on the P4 the sampling runs in a small task of its own ("stats",
 * aos_tasks_p4.c), there is no PMU (pmu_c and board_c stay NAN) and, with
 * no battery, no battery ring either. The simulator's are in sim/tasks_sim.c.
 * -------------------------------------------------------------------------- */

typedef struct {
    float    chip_c;            /* the chip's own sensor; NAN unknown       */
    float    pmu_c;             /* the AXP2101's die                        */
    float    board_c;           /* the NTC on the board, by the PMU         */
    uint32_t int_free, int_total, int_largest;    /* internal RAM, bytes    */
    uint32_t psram_free, psram_total;
    uint32_t exec_free, exec_total, exec_largest;  /* where apps' code goes  */
    int      cpu_load[2];       /* percent per core over the last second, -1 unknown */
} aos_sys_stats_t;

bool aos_hal_sys_stats(aos_sys_stats_t *out);

#define AOS_BATT_HIST_LEN   288         /* 24 h, one sample every 5 minutes */
#define AOS_BATT_HIST_NONE  0xFF        /* no sample for that slot          */
#define AOS_BATT_HIST_CHARGING 0x01     /* flags: was charging              */
#define AOS_BATT_HIST_USB      0x02     /* flags: on the cable (since BST2)  */

/* Oldest first, AOS_BATT_HIST_LEN slots ending now. Returns how many. */
int  aos_hal_batt_history(uint8_t *pct, uint8_t *flags, int max);
/* The same slots, the battery voltage in mV; 0 where unknown. */
int  aos_hal_batt_history_mv(uint16_t *mv, int max);

typedef enum {
    AOS_HIST_CHIP_T = 0,        /* tenths of a degree C */
    AOS_HIST_PMU_T,
    AOS_HIST_BOARD_T,
    AOS_HIST_INT_FREE_KB,
    AOS_HIST_PSRAM_FREE_KB,
    AOS_HIST_CPU0,              /* percent, averaged over the minute */
    AOS_HIST_CPU1,
    AOS_HIST_COUNT
} aos_hist_t;

#define AOS_MIN_HIST_LEN    60          /* one hour, one sample a minute */
#define AOS_HIST_NONE       INT16_MIN

/* Oldest first, AOS_MIN_HIST_LEN slots ending now. Returns how many. */
int  aos_hal_minute_history(aos_hist_t which, int16_t *out, int max);

/* --------------------------------------------------------------------------
 * P4OS: Monitor - the task list and the system's facts (aos.sysmon and the
 * portal's /api/sysmon). aos_tasks_p4.c on the board, sim/tasks_sim.c in
 * the simulator. Cheap enough to call once a second from the UI.
 * -------------------------------------------------------------------------- */

#define AOS_TASK_NAME_MAX   20
#define AOS_TASKS_MAX       64          /* more than the board ever runs */

typedef enum {
    AOS_TASK_RUNNING = 0,       /* on a core right now */
    AOS_TASK_READY,             /* waiting for a core  */
    AOS_TASK_BLOCKED,           /* waiting for an event, a queue, a delay */
    AOS_TASK_SUSPENDED,
    AOS_TASK_DELETED,           /* deleted, its memory not freed yet */
    AOS_TASK_UNKNOWN,
} aos_task_state_t;

typedef struct {
    char     name[AOS_TASK_NAME_MAX];
    uint32_t id;                /* FreeRTOS task number; the thread id in the simulator */
    int8_t   core;              /* pinned to core 0 or 1; -1 = runs on either */
    uint8_t  state;             /* aos_task_state_t */
    uint8_t  prio, base_prio;   /* current (may be inherited from a mutex) and base */
    bool     stack_psram;       /* its stack lives in PSRAM */
    int32_t  stack_size;        /* bytes; -1 unknown */
    int32_t  stack_min_free;    /* bytes: the least it ever had free (high-water mark); -1 unknown */
    int16_t  cpu_x10;           /* tenths of a percent of ONE core since the previous
                                 * refresh (at least half a second back); -1 unknown */
    uint64_t run_us;            /* run time since boot */
} aos_task_info_t;

/* Fills up to max, returns how many tasks there are (which may be more). */
int aos_hal_tasks(aos_task_info_t *out, int max);

/* heap_caps_* by capability, as the Monitor lists them. */
typedef enum {
    AOS_MEM_INTERNAL = 0,       /* MALLOC_CAP_INTERNAL */
    AOS_MEM_DMA,                /* MALLOC_CAP_DMA      */
    AOS_MEM_SPIRAM,             /* MALLOC_CAP_SPIRAM   */
    AOS_MEM_EXEC,               /* MALLOC_CAP_EXEC     */
    AOS_MEM_COUNT
} aos_mem_cap_t;

typedef struct {
    uint32_t free, total, largest;  /* bytes; largest = the biggest free block */
    uint32_t min_free;              /* the least free there has been since boot */
} aos_mem_region_t;

typedef struct {
    const char *chip;           /* "ESP32-P4" */
    int      chip_rev;          /* 301 = v3.1 */
    int      cores, cpu_mhz;
    const char *idf_version;
    /* why the last reset happened, as a key the UI translates: "power-on",
     * "software", "panic", "int-wdt", "task-wdt", "wdt", "brownout",
     * "deep-sleep", "external", "usb", "jtag", "efuse", "pwr-glitch",
     * "cpu-lockup", "unknown" */
    const char *reset_reason;
    uint32_t flash_bytes;
    const char *app_slot;       /* the partition it runs from: "ota_0", "ota_1", "factory" */
    uint32_t app_bytes;         /* the running image; 0 unknown */
    uint32_t app_slot_bytes;    /* that partition's size */
    aos_mem_region_t mem[AOS_MEM_COUNT];
    /* the Wi-Fi link, beyond aos_hal_net_* */
    uint32_t net_up_s;          /* how long the current connection has lasted; 0 without one */
    uint32_t net_drops;         /* times it has come down since boot */
    int      net_channel;       /* 0 unknown */
    char     net_bssid[18], net_mac[18];
    char     net_gw[16], net_mask[16], net_dns[16];
    int64_t  net_rx_bytes, net_tx_bytes;    /* -1: not counted by this firmware */
    const char *note;           /* NULL on the board; the simulator says what it makes up */
} aos_sys_info_t;

bool aos_hal_sys_info(aos_sys_info_t *out);

/* The microSD beyond aos_hal_sd_usage(). false without a card. */
typedef struct {
    char     name[8];           /* the card's product name, from its CID */
    const char *kind;           /* "SDSC", "SDHC", "SDXC", "MMC" */
    const char *fs;             /* "FAT12", "FAT16", "FAT32", "exFAT", "" */
    uint64_t capacity;          /* bytes, the whole card */
    uint32_t freq_khz;          /* the bus clock it runs at */
    int      bus_width;         /* 1 or 4 */
    int      manufacturer;      /* the CID's manufacturer id */
    int      year, month;       /* manufactured; 0 unknown */
} aos_sd_info_t;

bool aos_hal_sd_info(aos_sd_info_t *out);

/* What the PMU reports as it happens. Runs outside the LVGL task: a listener
 * that touches the UI takes aos_hal_lock() first. 'percent' is the battery at
 * the time of the event. */
typedef enum {
    AOS_POWER_USB_IN = 0,
    AOS_POWER_USB_OUT,
    AOS_POWER_CHARGE_DONE,
    AOS_POWER_LOW_BATTERY,      /* the warning level, once per discharge     */
    AOS_POWER_CRITICAL,         /* the shutdown level: the HAL powers off in a few seconds */
    AOS_POWER_OVERHEAT,
} aos_power_event_t;

void aos_hal_set_power_event_cb(void (*cb)(aos_power_event_t event, int percent));

/* Policies. All three are preferences and survive restarts.
 *
 * Power saving: the CPU drops to 80 MHz whenever the screen is not active and
 * no audio is running, and WiFi goes to its deepest modem sleep with the
 * screen off. It also switches itself on under 20% on battery.
 *
 * Battery care: charge to 4.1 V instead of 4.2 V and at half the current.
 * Costs some capacity per charge, buys many more charges.
 *
 * Panel sleep: with the screen off, the AMOLED's driver IC is put in sleep-in
 * instead of merely at brightness 0. Waking costs about a tenth of a second. */
void aos_hal_power_saving_enable(bool on);
bool aos_hal_power_saving_enabled(void);
void aos_hal_battery_care_enable(bool on);
bool aos_hal_battery_care_enabled(void);
void aos_hal_panel_sleep_enable(bool on);
bool aos_hal_panel_sleep_enabled(void);

/* Light sleep: with the screen off and no audio, the chip sleeps between
 * wake-ups (tickless idle). A finger, the BOOT button, WiFi and BLE wake it. */
void aos_hal_light_sleep_enable(bool on);
bool aos_hal_light_sleep_enabled(void);

/* With the screen not lit, let the touch controller fall into its own
 * auto-sleep (the v2's CST820). A touch still lights the screen through its
 * INT line. Counters for checking that from outside: interrupts seen, screens
 * lit by a touch, and whether the chip is in auto-sleep now. */
void aos_hal_touch_sleep_enable(bool on);
bool aos_hal_touch_sleep_enabled(void);
void aos_hal_touch_counters(uint32_t *isr, uint32_t *wakes, bool *chip_sleeping);

/* Deep sleep during the scheduled do-not-disturb hours (see aos_hal_esp32.c,
 * "Deep sleep at night"). A preference, off by default: while it sleeps the
 * phone's notifications and the steps stop, and waking is a full boot. */
void aos_hal_night_sleep_enable(bool on);
bool aos_hal_night_sleep_enabled(void);
/* Who may veto it or bring the wake-up forward (the alarms, from main.c):
 * return false to stay awake, or lower *wake_by (an epoch) to be up by then. */
void aos_hal_set_night_guard_cb(bool (*cb)(int64_t *wake_by));
/* An app with state a boot would lose (a countdown, a stopwatch) holds it
 * off while it matters. Idempotent; 'who' must be a string literal. */
void aos_hal_sleep_hold(const char *who, bool hold);
/* Counters since power-on; last_wake 1 = end of the night, 2 = touch or
 * BOOT, 3 = USB. */
void aos_hal_night_info(uint32_t *nights, uint32_t *chunks, uint32_t *slept_s,
                        int64_t *last_start, int64_t *last_end, uint8_t *last_wake);
/* The energy fields of /api/status as a JSON fragment (leading comma). */
int  aos_hal_power_json(char *out, size_t len);
/* Bench only: the home network gone for N seconds (see aos_hal_esp32.c). */
void aos_hal_net_test_absent(uint32_t seconds, bool idle);
/* Bench only: a "night" of N seconds right now. */
void aos_hal_night_test(uint32_t seconds);

/* Meant to switch the gyroscope off while no app needs it. On the board the
 * accel-only mode broke the accelerometer (see qmi8658.c), so for now the
 * gyro stays on and this only counts. Counted: every request(true) needs its
 * request(false). */
void aos_hal_imu_gyro_request(bool on);

/* The PMU's regulators and raw registers. EXPERIMENTS ONLY, behind the web
 * portal's /api/pmu: nothing in the firmware switches a rail by itself. See
 * docs/POWER.md section 6. */
int       aos_hal_pmu_rail_count(void);
bool      aos_hal_pmu_rail_get(int idx, const char **name, bool *on, int *mv);
int       aos_hal_pmu_rail_find(const char *name);
bool      aos_hal_pmu_rail_set(int idx, bool on);
int       aos_hal_pmu_register_read(int reg);                 /* -1 on failure */
bool      aos_hal_pmu_register_write(int reg, int value);
float     aos_hal_pmu_ts_voltage(void);
void      aos_hal_pm_dump_locks(void);      /* who is holding the CPU at 240, to the log */
int       aos_hal_pm_dump_text(char *out, size_t len);   /* the same, as text */
void      aos_hal_panel_hw_reset(void);     /* LCD_RESET low: the panel forgets everything */
const char *aos_hal_boot_reason(void);      /* why the ESP32 last reset, static string */
/* Who answers on the I2C bus right now, plus one accelerometer sample, as
 * JSON. The rail experiment's yardstick. */
int       aos_hal_probe_devices(char *out, size_t len);

/* -------------------------------------------------------------------------- */
/* Storage                                                                     */
/* -------------------------------------------------------------------------- */

const char *aos_hal_path_apps(void);        /* .so of dynamic apps     */
const char *aos_hal_path_photos(void);      /* photo viewer            */
const char *aos_hal_path_music(void);
const char *aos_hal_path_recordings(void);  /* wav from the recorder */
const char *aos_hal_path_data(void);        /* app state               */
const char *aos_hal_path_scans(void);       /* network surveys         */
const char *aos_hal_path_sd_root(void);     /* the card's mount point, NULL without card */

/* Disk mode (branch usb, docs/USB.md D2): the card leaves the watch while a
 * computer has it. release() unmounts it and shuts the SD host so aos_usb
 * can take the raw sectors; reclaim() mounts it again the usual way.
 * mark_mounted() is for the interval in between, when the USB side mounts
 * it back for the watch (the computer ejected it) or takes it again. The
 * simulator has no card to lend: release() says false. */
bool aos_hal_sd_release(void);
bool aos_hal_sd_reclaim(void);
void aos_hal_sd_mark_mounted(bool mounted);

/* Language packs. Unlike those above, this one does NOT fall back to SPIFFS:
 * packs live on the card only. That is not a limitation but the design
 * decision - the base language is the one in the source, so "no card" means
 * "Spanish", which is exactly the right fallback and takes no writing at all
 * to get. See docs/I18N.md. */
const char *aos_hal_path_lang(void);

/* Icon files, <desc.id>.aic (docs/ICONS.md). Card when there is one, SPIFFS
 * otherwise, like the app directories: an icon dropped here overrides the
 * one the app brought, or the firmware's own. */
const char *aos_hal_path_icons(void);

/* The launcher's order and folders, menu.txt (docs/MENU.md). On the card when
 * there is one, SPIFFS otherwise: it goes with the apps it arranges. A full
 * path to a file, not a directory. */
const char *aos_hal_path_menu(void);

bool aos_hal_sd_present(void);
bool aos_hal_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes);

/* A buffer the card can read into and write from directly, for copies,
 * uploads and downloads. On the P4 it is PSRAM on a 128-byte line: the SDMMC
 * DMA takes PSRAM only so aligned, and a plain malloc() made every write go
 * through a bounce buffer - 1,2 MB/s instead of ~9 (2026-09-29). Free it with
 * aos_hal_io_free(). */
void *aos_hal_io_alloc(size_t bytes);
void  aos_hal_io_free(void *p);

/* ---- P4OS: a folder in one pass (Archivos) ----
 * Every entry of a folder with its size and date, read in ONE walk of the
 * directory. On FAT a stat() per file walks the directory again from the
 * start, so listing n files and then stat()ing each is O(n^2): tens of
 * seconds for a few thousand. On the board, under the card, this reads
 * FatFs's own directory records; elsewhere (and in the simulator) it is
 * readdir + stat. The callback returns false to stop. Hidden entries and
 * "." / ".." are the caller's to skip. Returns how many were seen, -1 when
 * the folder cannot be opened. */
typedef struct {
    const char *name;
    bool        dir;
    uint32_t    size;       /* bytes; 0 for folders */
    uint32_t    mtime;      /* epoch seconds, 0 unknown */
} aos_dir_entry_t;
typedef bool (*aos_dir_cb_t)(const aos_dir_entry_t *e, void *ctx);
int aos_hal_dir_scan(const char *path, aos_dir_cb_t cb, void *ctx);
/* ---- end of the folder in one pass ---- */

/* -------------------------------------------------------------------------- */
/* Persistent settings (NVS on the board, a file in the simulator)             */
/* -------------------------------------------------------------------------- */

bool aos_hal_pref_get_i32(const char *key, int32_t *out);
bool aos_hal_pref_set_i32(const char *key, int32_t value);
bool aos_hal_pref_get_str(const char *key, char *out, size_t out_len);
bool aos_hal_pref_set_str(const char *key, const char *value);
bool aos_hal_pref_erase(const char *key);

/* Every stored preference, in no particular order, for the portal's backup.
 * is_str says which of v and s holds the value. Returns how many. */
typedef void (*aos_hal_pref_visit_t)(const char *key, bool is_str, int32_t v,
                                     const char *s, void *ctx);
int  aos_hal_pref_foreach(aos_hal_pref_visit_t visit, void *ctx);

/* -------------------------------------------------------------------------- */
/* Audio (P4OS: ES8311 + speaker, ES7210 + two microphones; aos_audio_p4.c)   */
/* -------------------------------------------------------------------------- */

void aos_hal_beep(int freq_hz, int ms);             /* synthesised tone */
bool aos_hal_play_file(const char *path);           /* wav/mp3 async    */
void aos_hal_audio_stop(void);
bool aos_hal_audio_is_playing(void);
int  aos_hal_volume_get(void);                      /* 0..100 */
void aos_hal_volume_set(int percent);

/* Instantaneous microphone level, 0..100. Valid while the capture is open,
 * whether from the recorder or from aos_hal_mic_open(). Outside that, 0. */
int  aos_hal_mic_level(void);

/* --------------------------------------------------------------------------
 * Streaming speaker
 *
 * PCM in, sound out, for audio that is not a file: the walkie-talkie's
 * frames off the air. P4OS: it opens alongside a running microphone (two
 * codecs, full duplex); write() drops samples into a ring of one second in
 * PSRAM and never blocks; the board's mixer takes them 10 ms at a time,
 * resampled to its fixed 48 kHz bus, and plays silence when the ring is
 * empty, so the amplifier stays warm between frames. (On the watch the
 * speaker and the microphone were one ES8311 and open() failed while the
 * microphone held it.) queued() is what is waiting, for whoever wants a
 * jitter buffer. close() stops the task and gives the codec back.
 * -------------------------------------------------------------------------- */
bool aos_hal_spk_open(uint32_t sample_rate);        /* mono, 16-bit */
int  aos_hal_spk_write(const int16_t *pcm, int n);  /* samples accepted */
int  aos_hal_spk_queued(void);
bool aos_hal_spk_is_open(void);
void aos_hal_spk_close(void);

/* --------------------------------------------------------------------------
 * Raw microphone
 *
 * PCM samples straight out of the microphone (the ES7210 on P4OS, one of its
 * two mics, resampled from the 48 kHz bus to the rate asked), recording
 * nothing. Needed by
 * anything that analyses sound instead of merely measuring it: tuner, dB(A),
 * spectrum.
 *
 * The capture runs in its own task and leaves the audio in a one-second ring
 * in PSRAM; the app drains it from its tick and **never blocks**. It is the
 * same shape as aos_hal_rec_peaks(), one level down, and for the same reason:
 * reading 2048 samples at 16 kHz in a blocking fashion is 128 ms with the
 * screen frozen.
 *
 * Three things worth knowing before writing the app:
 *
 * 1. Speaker and microphone are the SAME codec (see the arbitration further up
 *    in aos_hal_esp32.c). While the capture is open, aos_hal_beep() and
 *    aos_hal_play_file() make no sound. A tuner cannot play the reference tone
 *    and listen at the same time: they are exclusive modes, and switching
 *    between them costs ~200 ms.
 * 2. Opening is asynchronous. aos_hal_mic_open() returns straight away and the
 *    codec is opened in the task; until aos_hal_mic_status().open is true
 *    there are no samples. That is not an error, that is the warm-up.
 * 3. The recorder and this share ONE capture. Whoever opens first fixes the
 *    rate; the second hangs off the one already running, so look at
 *    'sample_rate' in the status rather than assuming you got what you asked
 *    for.
 * -------------------------------------------------------------------------- */

#define AOS_MIC_RATE_HZ     16000   /* the default: voice and instruments */

typedef struct {
    bool     open;              /* the codec really is capturing */
    uint32_t sample_rate;       /* the real one, may not be the requested one */
    int      gain_db;           /* PGA that was applied */
    int      level;             /* 0..100, the same dBFS mapping as the VU */
    int      peak;              /* raw peak of the last block, 0..32767 */
    uint32_t dropped;           /* samples lost by not draining in time */
    uint32_t reserved[4];       /* headroom so already-compiled apps do not break */
} aos_mic_status_t;

/* Opens the capture. 'sample_rate' of 0 uses AOS_MIC_RATE_HZ. */
bool aos_hal_mic_open(uint32_t sample_rate);
void aos_hal_mic_close(void);

/* Copies up to 'max' new samples and returns how many it copied (0 if there
 * are none yet). If the app is slower than the ring the oldest are lost, and
 * that is counted in 'dropped': the audio jumps but the app does not fall
 * behind forever. */
int  aos_hal_mic_read(int16_t *out, int max);
int  aos_hal_mic_available(void);       /* samples waiting */

bool aos_hal_mic_status(aos_mic_status_t *out);

/* Analogue gain of the microphone, in dB, rounded to the PGA's step. P4OS:
 * the ES7210's, 3 dB steps 0..36 without 33 (the driver cannot reach it); the
 * watch's ES8311 moved in steps of 6 (0..42). It is NOT kept across restarts: it returns to the
 * firmware's factory value, so an app that turns it down does not leave every
 * recording quiet forever. An app that needs it fixed has to store it in a
 * preference of its own and reapply it on open.
 *
 * Any absolute measurement (dB SPL) depends on this: without knowing the gain,
 * a calibration is worthless. That is why it is in the status. */
void aos_hal_mic_gain_set(int db);
int  aos_hal_mic_gain_get(void);

/* --------------------------------------------------------------------------
 * Recorder
 *
 * Captures the microphone into a 16-bit mono PCM WAV on the microSD. The
 * capture runs in its own task, so these calls return straight away.
 *
 * It is the SAME capture as aos_hal_mic_open(): one task, two consumers. You
 * can record and analyse at once, and whoever opens first fixes the rate
 * (aos_hal_rec_status() says which one it ended up being).
 *
 * Besides the audio, the HAL keeps the envelope: the peak of each block of
 * 1/AOS_REC_PEAK_HZ seconds, in a ring the app drains with
 * aos_hal_rec_peaks(). This is done here and not in the app because an app
 * polling the level once a frame misses the peaks between polls, and a
 * waveform with holes in it says nothing.
 * -------------------------------------------------------------------------- */

#define AOS_REC_PEAK_HZ     20      /* envelope samples per second */
#define AOS_REC_RATE_HZ     16000   /* default rate: voice */

typedef enum {
    AOS_REC_IDLE = 0,
    AOS_REC_RECORDING,
    AOS_REC_PAUSED,
} aos_rec_state_t;

typedef struct {
    aos_rec_state_t state;
    char     path[160];
    uint32_t elapsed_ms;        /* time recorded, not counting pauses */
    uint32_t bytes;             /* audio written, without the header  */
    uint32_t sample_rate;
    uint8_t  channels;
    int      level;             /* 0..100, peak of the last block     */
} aos_rec_status_t;

/* Starts the capture. 'sample_rate' of 0 uses AOS_REC_RATE_HZ. */
bool aos_hal_rec_start(const char *path, uint32_t sample_rate);
void aos_hal_rec_pause(void);
void aos_hal_rec_resume(void);
bool aos_hal_rec_stop(void);            /* closes the wav; false if nothing was left */
bool aos_hal_rec_status(aos_rec_status_t *out);

/* Drains the envelope added since the last call. Returns how many samples it
 * copied into 'out' (0..max). If the app is slower than the ring the oldest
 * are lost: the waveform jumps, but the audio is untouched. */
int  aos_hal_rec_peaks(uint8_t *out, int max);

/* --------------------------------------------------------------------------
 * Local file player
 *
 * Plays whatever is on the microSD. Decoding runs in its own task, so these
 * calls return straight away.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_PLAYER_STOPPED = 0,
    AOS_PLAYER_PLAYING,
    AOS_PLAYER_PAUSED,
} aos_player_state_t;

typedef struct {
    aos_player_state_t state;
    char     path[160];
    char     title[64];
    uint32_t duration_s;
    uint32_t position_s;
    uint32_t sample_rate;
    uint8_t  channels;
} aos_player_status_t;

bool aos_hal_player_play(const char *path);
void aos_hal_player_pause(void);
void aos_hal_player_resume(void);
void aos_hal_player_stop(void);
bool aos_hal_player_status(aos_player_status_t *out);

/* 16-bit PCM WAV and MP3 (any bitrate, CBR or VBR, 8 to 48 kHz), played
 * mono through the one speaker. The decoder keeps two seconds ahead in
 * PSRAM and runs under LVGL's priority while it can afford to: an app in
 * front that keeps both cores busy for longer than that is where the music
 * gives way. An app that opens the streaming speaker (aos_hal_spk_open)
 * pauses the music until it closes it.
 *
 * aos_hal_player_play() plays one file and stops (a video's sound, a
 * ringtone). aos_hal_player_play_folder() plays 'path' and then the rest of
 * its folder in name order, round and round, with or without the app that
 * started it. */
bool aos_hal_player_play_folder(const char *path);
void aos_hal_player_next(void);
void aos_hal_player_prev(void);                 /* the start first if > 3 s in */
void aos_hal_player_seek(uint32_t ms);
void aos_hal_player_set_shuffle(bool on);

typedef struct {
    aos_player_state_t state;
    char        path[256];
    char        title[96];      /* the tag, or the file name after " - "    */
    char        artist[96];     /* the tag, or the file name before " - "   */
    char        album[64];
    const char *format;         /* "MP3", "WAV" or ""                       */
    uint16_t    kbps;
    bool        vbr;
    uint32_t    sample_rate;
    uint8_t     channels;       /* the file's; the speaker gets them mixed  */
    uint32_t    duration_ms;
    uint32_t    position_ms;
    int         index;          /* in the folder, -1 for a single file      */
    int         count;
    bool        shuffle;
    bool        yielded;        /* paused because an app took the speaker   */
    bool        has_cover;      /* an embedded picture                      */
    uint32_t    cover_offset;   /* where its bytes are in the file, and how */
    uint32_t    cover_size;     /* many (JPEG or PNG, see cover_mime)       */
    bool        live;           /* an internet radio (aos_hal_radio_play)   */
    uint8_t     pad_[3];
    uint32_t    reserved[5];
} aos_player_info_t;

bool aos_hal_player_info(aos_player_info_t *out);

/* The last folder track heard and where it was, remembered across restarts
 * (saved every minute, on pause, on stop and on each new track). False if
 * there is none or the file is gone. resume_last() plays it from there. */
bool aos_hal_player_last(char *path, size_t len, uint32_t *position_ms);
bool aos_hal_player_resume_last(void);

/* Settings -> Sound: an app that opens the streaming speaker while music
 * plays is mixed over it (the music at half volume) instead of pausing it.
 * Off by default. Never for the walkie-talkie, nor while the microphone is
 * open. */
bool aos_hal_player_mix(void);
void aos_hal_player_set_mix(bool on);

/* The UI tells the HAL which app is in front (NULL: none), before create().
 * For the audio policy only: today, whether it is the walkie-talkie. */
void aos_hal_audio_foreground(const char *app_id);

/* How the pipeline is doing, for /api/player and the measurements. */
typedef struct {
    uint32_t ring_ms;           /* decoded and waiting                      */
    uint32_t ring_cap_ms;
    uint32_t ring_min_ms;       /* the lowest it got since play             */
    uint32_t underruns;         /* times the writer found it empty          */
    uint32_t load_permille;     /* reading + decoding, per second of audio  */
    uint32_t decode_permille;   /* decoding alone                           */
    uint32_t chunk_us_max;      /* the slowest read+decode of 1152 frames   */
    int      decoder_prio;      /* 2 with room to spare, 5 when behind      */
    uint32_t stack_free_dec;    /* bytes                                    */
    uint32_t stack_free_out;
    uint32_t reserved[6];
} aos_player_stats_t;

bool aos_hal_player_stats(aos_player_stats_t *out);

/* --------------------------------------------------------------------------
 * Internet radio (branch radio)
 *
 * A station is played BY THE PLAYER, the same decoder, ring and writer as a
 * file: it goes on with the app closed, the control centre and /api/player
 * drive it, an app that opens the speaker pauses it. MP3 and AAC (LC, HE,
 * HE v2; since v0.8.1) over http:// or https:// (Icecast, Shoutcast, most
 * station CDNs), redirects and .pls / .m3u playlists followed, and HLS
 * (.m3u8 with MPEG-TS or packed-audio segments, since v0.8.1). Ogg, Opus,
 * FLAC, fMP4 and encrypted HLS are refused with a reason.
 *
 * aos_hal_radio_play() takes the whole list of stations and the one to play:
 * aos_hal_player_next() / _prev() then move along it, from the app or from
 * anywhere else. While a station plays, aos_hal_player_info() says so:
 * 'live' is true, 'album' is the station's name, 'title' and 'artist' come
 * from the stream's StreamTitle ("Artist - Title"), 'index' / 'count' are
 * the station's place in the list and 'duration_ms' is 0.
 *
 * Pausing keeps the connection while the buffer holds (about ten seconds at
 * 128 kbps); a pause longer than that resumes live, not where it was.
 * -------------------------------------------------------------------------- */

#define AOS_RADIO_MAX_STATIONS 16

typedef struct {
    char name[48];
    char url[256];
} aos_radio_station_t;

bool aos_hal_radio_play(const aos_radio_station_t *list, int count, int index);
bool aos_hal_radio_active(void);        /* a station is the player's source */

typedef enum {
    AOS_RADIO_OFF = 0,
    AOS_RADIO_CONNECTING,       /* resolving, connecting, TLS, headers      */
    AOS_RADIO_BUFFERING,        /* connected, filling before sound          */
    AOS_RADIO_PLAYING,
    AOS_RADIO_RETRYING,         /* the connection dropped: trying again     */
    AOS_RADIO_FAILED,           /* given up; see 'error'                    */
} aos_radio_state_t;

typedef struct {
    aos_radio_state_t state;
    int      index;             /* in the list given to aos_hal_radio_play  */
    int      count;
    char     station[48];       /* the list's name for it                   */
    char     url[256];          /* the list's URL                           */
    char     host[64];          /* where the audio really comes from        */
    bool     tls;
    char     icy_name[64];      /* what the station says of itself          */
    char     icy_genre[48];
    char     icy_url[96];
    char     icy_desc[96];
    char     content_type[32];
    char     title[128];        /* StreamTitle as heard, UTF-8              */
    uint32_t title_gen;         /* +1 each time the heard title changes     */
    uint16_t kbps;              /* icy-br, else the first frame's           */
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t buffer_ms;         /* bytes received and not yet decoded       */
    uint32_t bytes;             /* audio bytes received since play          */
    uint32_t reconnects;
    uint32_t listening_s;       /* since play, pauses included              */
    char     error[64];         /* why FAILED or RETRYING, in English       */
    char     codec[12];         /* "MP3", "AAC", "HE-AAC", "HE-AACv2" (v0.8.1) */
    bool     hls;               /* segments from an .m3u8, not a stream     */
    uint8_t  pad_[3];
    uint32_t reserved[4];
} aos_radio_status_t;

bool aos_hal_radio_status(aos_radio_status_t *out);

/* --------------------------------------------------------------------------
 * Remote control of the phone's music
 *
 * NOTE: the ESP32-S3 has no Bluetooth Classic (soc_caps.h says so:
 * SOC_BLE_SUPPORTED yes, SOC_BT_CLASSIC_SUPPORTED no), so A2DP and AVRCP are
 * out. What there is today is AMS (Apple Media Service, over BLE): the
 * commands and the track's title, artist and album, from an iPhone only.
 *
 * For Android the plan is BLE HID media keys, which need no app on the phone:
 * controls with no information, which is what has_metadata = false already
 * means. Planned, not scheduled: docs/ROADMAP.md.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_MEDIA_OFF = 0,
    AOS_MEDIA_ADVERTISING,
    AOS_MEDIA_CONNECTED,
} aos_media_link_t;

typedef enum {
    AOS_MEDIA_PLAY_PAUSE = 0,
    AOS_MEDIA_NEXT,
    AOS_MEDIA_PREV,
    AOS_MEDIA_VOL_UP,
    AOS_MEDIA_VOL_DOWN,
} aos_media_cmd_t;

typedef struct {
    char     title[64];
    char     artist[64];
    char     album[64];
    bool     playing;
    bool     has_metadata;      /* false = controls only, no information */
    uint32_t position_s;
    uint32_t duration_s;
} aos_media_info_t;

void             aos_hal_media_enable(bool enable);
bool             aos_hal_media_enabled(void);
aos_media_link_t aos_hal_media_link(void);
const char      *aos_hal_media_peer(void);      /* the phone's name */

/* Which application is playing: "Musica", "Spotify". "" if unknown.
 *
 * A separate function and NOT one more field of aos_media_info_t, because that
 * structure is exported to dynamic apps: an already-compiled .so reserves it
 * on its stack at the old size, and adding a field would make the firmware
 * write past that buffer. Adding a function is free; growing a shared
 * structure is not. */
const char      *aos_hal_media_player(void);
bool             aos_hal_media_info(aos_media_info_t *out);
bool             aos_hal_media_command(aos_media_cmd_t cmd);

/* --------------------------------------------------------------------------
 * Bluetooth link with the phone
 *
 * One phone at a time, paired and with the keys stored. Everything the watch
 * takes from the phone hangs off this same connection: notifications (ANCS),
 * the music and its controls (AMS), the phone's time and battery. That is why
 * the link is a thing of its own and not a part of notifications.
 *
 * NONE of this names ANCS: the day there is an Android on the other side, the
 * provider changes inside the HAL and the UI never finds out. The plan is in
 * docs/ROADMAP.md.
 *
 * Turning it on costs ~30 KB of executable RAM, which is where the code of
 * dynamic apps comes from: this switch is also an app switch, like the WiFi
 * one. Turning it off gives it all back. Measured, in
 * docs/HANDOFF-BLE-ANCS.md section 2.4.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_BT_OFF = 0,
    AOS_BT_ADVERTISING,     /* visible, waiting for the phone to come */
    AOS_BT_PAIRING,         /* a code is on screen waiting for an answer */
    AOS_BT_CONNECTED,
} aos_bt_state_t;

void           aos_hal_bt_enable(bool on);  /* preference, survives restarts */
bool           aos_hal_bt_enabled(void);
aos_bt_state_t aos_hal_bt_state(void);
const char    *aos_hal_bt_peer(void);       /* "iPhone de Charlie", or "" */
bool           aos_hal_bt_bonded(void);     /* there are stored keys */

/* The phone's battery, 0..100. false if not known yet.
 *
 * It comes from the Battery Service the iPhone itself publishes, not from
 * ANCS. Besides being read on connect, the notification is subscribed to, so
 * it keeps itself up to date. */
bool           aos_hal_bt_phone_battery(int *percent);
void           aos_hal_bt_forget(void);     /* wipes them and disconnects */

/* Pairing by numeric comparison: both devices show the same six-digit number
 * and each confirms that they match. It is the most secure of the modes this
 * board allows -there is a screen and there is touch- and it is also the only
 * one where the user types nothing.
 *
 * The UI opens the screen, calls _pair_begin() and asks _pair_code() on every
 * tick until it returns something other than zero. */
void     aos_hal_bt_pair_begin(void);
uint32_t aos_hal_bt_pair_code(void);        /* 0 = no code yet */
void     aos_hal_bt_pair_confirm(bool accept);
void     aos_hal_bt_pair_cancel(void);

/* P4OS: the board as a computer's Bluetooth keyboard, mouse and media keys
 * (HID over GATT, components/aos_ble/aos_ble_hid.c). With it on, a Mac or a
 * PC pairs from its own Bluetooth settings while the phone stays connected,
 * and aos_hal_usb_key/type/mouse/click go over Bluetooth when no computer is
 * on the USB cable. Off by default; switching it restarts Bluetooth. */
void        aos_hal_bt_keyboard_enable(bool on);   /* preference, survives restarts */
bool        aos_hal_bt_keyboard_enabled(void);
const char *aos_hal_bt_keyboard_host(void);        /* the connected computer's name, "" when none */
bool        aos_hal_bt_keyboard_ready(void);       /* a computer listens to the keyboard */

/* --------------------------------------------------------------------------
 * Phone notifications
 *
 * The provider (NimBLE on the board, an imaginary phone in the simulator)
 * pushes whatever arrives; the shared store in aos_notif.c applies the policy
 * and leaves the result in a queue. The UI empties it from aos_ui_tick().
 *
 * That it is a queue and not a callback is not a matter of style: NimBLE's
 * callback runs in the BLE host task and CANNOT touch LVGL. The only place
 * where a notification turns into pixels is the tick, which already runs with
 * the lock held.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_NOTIF_OTHER = 0,
    AOS_NOTIF_CALL_INCOMING,
    AOS_NOTIF_CALL_MISSED,
    AOS_NOTIF_VOICEMAIL,
    AOS_NOTIF_SOCIAL,
    AOS_NOTIF_SCHEDULE,
    AOS_NOTIF_EMAIL,
    AOS_NOTIF_NEWS,
    AOS_NOTIF_HEALTH,
    AOS_NOTIF_FINANCE,
    AOS_NOTIF_LOCATION,
    AOS_NOTIF_ENTERTAINMENT,

    AOS_NOTIF_CATEGORY_COUNT
} aos_notif_category_t;

typedef struct {
    uint32_t             uid;           /* identifier given by the phone        */
    aos_notif_category_t category;
    char                 app[40];       /* "WhatsApp"                           */
    char                 title[64];
    char                 message[256];
    time_t               when;
    bool                 important;
    bool                 silent;        /* the phone sent it silently           */
    bool                 pre_existing;  /* was already on the phone on connect  */
    /* The two actions are INDEPENDENT and have to be honoured separately.
     *
     * Measured against the iPhone: an incoming call arrives with both
     * (flags=0x1A) and both work — answering starts the call on the phone and
     * rejecting hangs it up. A WhatsApp message, on the other hand, arrives
     * with the negative one alone, and asking it for the positive returns
     * 0xA3.
     *
     * Collapsing them into a single "supports actions" made the watch offer
     * Accept where the phone had not announced it. A button the other side did
     * not declare is not drawn. */
    bool                 can_positive;
    bool                 can_negative;

    /* How many arrived in a row from the same app with the same title. 1 is
     * the first. WhatsApp sends ONE NOTIFICATION PER MESSAGE, so a live
     * conversation is eight full screens in forty seconds, and the category
     * filter does not help because they all land in "Messages". */
    uint16_t             repeticiones;

    /* Decided by the store, not the UI: this is where "do not disturb", the
     * category filter, the silence the phone itself sends and the exception
     * for calls all come together. Keeping the policy in one place is what
     * stops the UI and the provider from disagreeing. */
    bool                 alert;         /* show it full screen                 */
    bool                 sound;         /* and make a noise as well            */
} aos_notif_t;

/* Anything new. Returns false if there is nothing; consumes it. */
bool aos_hal_notif_pop(aos_notif_t *out);

/* One the phone withdrew. If it is the one on screen, it closes itself. */
bool aos_hal_notif_pop_removed(uint32_t *uid);

/* History, newest to oldest. Lives in PSRAM. */
int  aos_hal_notif_count(void);
bool aos_hal_notif_at(int index, aos_notif_t *out);
/* Deletes a single one. Used by the list -to drop the one just read- and by
 * the phone itself when it dismisses it over there: rejecting an alert makes
 * the phone dismiss it, the phone says so, and it disappears from the list
 * too. A watch showing alerts that are no longer on the phone is a watch that
 * lies. */
bool aos_hal_notif_remove(uint32_t uid);

void aos_hal_notif_clear(void);

/* Accept or reject: answer or hang up a call, dismiss from the phone. Only
 * meaningful with 'can_positive' / 'can_negative'. */
bool aos_hal_notif_action(uint32_t uid, bool positive);

/* The phone may accept the action and be unable to carry it out. Returns true
 * ONCE if the last action failed, so the interface can say so instead of
 * closing the screen as if it had gone well. */
bool aos_hal_notif_action_failed(void);

/* --------------------------------------------------------------------------
 * Notification settings. All persisted in NVS.
 *
 *   enabled=false   "do not disturb": the link stays alive -so music control
 *                   keeps working and there is no need to re-pair- but
 *                   nothing pops up. They are still stored in the history. To
 *                   shut the radio down there is aos_hal_bt_enable(false).
 *   sound=false     they are seen, not heard.
 *   calls_always    incoming calls skip both of the above and the filter as
 *                   well. It is the one category where not finding out has a
 *                   real cost.
 *   categories      bit mask, 1 << aos_notif_category_t. Anything unticked is
 *                   discarded entirely: no alert and no history.
 *
 * The silence the phone itself sends (focus mode) is always honoured: the user
 * on the other side already decided that.
 * -------------------------------------------------------------------------- */

void     aos_hal_notif_enable(bool on);
bool     aos_hal_notif_enabled(void);
void     aos_hal_notif_sound_set(bool on);
bool     aos_hal_notif_sound(void);
void     aos_hal_notif_calls_always_set(bool on);
bool     aos_hal_notif_calls_always(void);
/* Do not disturb on a schedule, on top of the switch (aos_hal_notif_enable
 * false is do not disturb by hand). Minutes after midnight; the window may
 * cross midnight. Default 23:00 to 07:00, off. */
void     aos_hal_notif_dnd_schedule_set(bool on, int from_min, int to_min);
void     aos_hal_notif_dnd_schedule_get(bool *on, int *from_min, int *to_min);
/* Right now: by hand, or inside the scheduled hours. */
bool     aos_hal_notif_dnd_active(void);
void     aos_hal_notif_categories_set(uint32_t mask);
uint32_t aos_hal_notif_categories(void);

/* -------------------------------------------------------------------------- */
/* Network                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    AOS_NET_OFF = 0,
    AOS_NET_CONNECTING,
    AOS_NET_CONNECTED,
    AOS_NET_FAILED,
} aos_net_state_t;

aos_net_state_t aos_hal_net_state(void);

/* WiFi without power save, for as long as an app streams (branch rtsp).
 * With the screen on the HAL keeps the radio in WIFI_PS_MIN_MODEM, which
 * wakes it at each beacon: 180-315 ms of round trip to the watch, measured,
 * and a TCP stream through a 16 KB window then crawls. The Cameras app
 * holds this while a camera is open. It costs the radio's idle current, so
 * release it: the app's stop path, and the HAL clears it when the app's
 * worker stops. Refused quietly while Bluetooth is up (coexistence needs
 * modem sleep). */
void aos_hal_net_low_latency(bool on);
const char *aos_hal_net_ssid(void);
int         aos_hal_net_rssi(void);         /* dBm */
const char *aos_hal_net_ip(void);
/* P4OS: mDNS on the Wi-Fi station: answering and announcing the board's name
 * there, or quiet (the portal's rules keep it quiet on networks that are not
 * home, aos_access.c). The AP and the USB cable are not affected. */
void aos_hal_net_mdns_visible(bool on);
void        aos_hal_net_enable(bool on);
bool        aos_hal_net_enabled(void);   /* preference, survives restarts */
/* The station's reconnection pacing: failures since boot, whether it has
 * stopped retrying until the screen or USB (on battery, screen off), the
 * delay it is waiting now, and the driver's last disconnect reason. */
void        aos_hal_net_retry_info(uint32_t *failures, bool *parked, uint32_t *next_s,
                                   uint8_t *reason);
bool        aos_hal_net_sync_time(void);    /* SNTP, non-blocking */

/* Are there stored credentials? If not, trying to connect is pointless. */
bool aos_hal_net_has_credentials(void);

/* Stores the network and reconnects with it. Pass pass NULL or "" for an open network. */
bool aos_hal_net_set_credentials(const char *ssid, const char *pass);
void aos_hal_net_forget(void);

/* -------------------------------------------------------------------------- */
/* Onboarding: the board brings up its own access point                        */
/*                                                                             */
/* With no credentials there is no way to load credentials: the web portal
 * needs a network. The circle is broken by bringing up an AP of our own, which
 * you join from the phone to fill in a form. The mode is APSTA, so the same
 * device can scan the networks around it while serving its own page.          */
/* -------------------------------------------------------------------------- */

bool        aos_hal_net_ap_start(void);     /* brings up the setup AP */
void        aos_hal_net_ap_stop(void);
bool        aos_hal_net_ap_active(void);
const char *aos_hal_net_ap_ssid(void);      /* AmoledOS-XXXX, or the chosen one */
const char *aos_hal_net_ap_pass(void);
const char *aos_hal_net_ap_ip(void);        /* nearly always 192.168.4.1 */
int         aos_hal_net_ap_clients(void);   /* P4OS: devices on it now, 0 when it is down */
/* P4OS: the Wi-Fi co-processor's firmware (the C6's esp_hosted), "1.4.3";
 * "" until the radio is up, "?" when it would not say. Asked once at boot. */
const char *aos_hal_net_coprocessor_fw(void);
/* P4OS, a test: freezes the task that takes what the C6 sends (esp_hosted's
 * sdio_process_rx), so nothing comes back from it while the SDIO bus stays
 * sound - the way the link died on its own during two OTAs. The link
 * watchdog (aos_tasks_p4.c) should restart the board within ~30 s. false
 * where there is no such task. */
bool        aos_hal_net_test_freeze_link(void);

/* P4OS: a new firmware for the co-processor (the C6), from a file: an
 * esp_hosted co-processor image for the ESP32-C6 (tools/build_c6.sh,
 * docs/C6.md). The file is checked first; then a task of its own sends it
 * to the C6 over SDIO, in the slot the C6 is not running, and the board
 * restarts to meet the new firmware. The Wi-Fi is gone while it runs
 * (about a minute). */
typedef enum {
    AOS_C6_IDLE = 0,
    AOS_C6_SENDING,
    AOS_C6_DONE,            /* sent; the board is restarting */
    AOS_C6_FAILED,
} aos_c6_state_t;

typedef struct {
    aos_c6_state_t state;
    uint32_t sent, total;   /* bytes */
    char     version[32];   /* the image's */
    char     error[64];
} aos_c6_update_t;

/* The image's version into 'version' ("" when the file is not one). */
bool aos_hal_net_coprocessor_image(const char *path, char *version, size_t n);
bool aos_hal_net_coprocessor_update(const char *path);
void aos_hal_net_coprocessor_status(aos_c6_update_t *out);

/* --------------------------------------------------------------------------
 * AP name and password
 *
 * The three getters above ALWAYS answer, whether the AP is up or not: the
 * default name comes from the factory MAC, which is read without turning the
 * radio on. That is what makes it possible to show the password and the QR on
 * screen before touching anything.
 *
 * Two password modes. FIXED is the usual one: stored and unchanging. ROTATING
 * generates a new one EVERY TIME THE AP COMES UP - not per client that
 * associates: while the AP is up there is a single password, so the QR on the
 * screen stays valid and several phones can join.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_AP_PASS_FIXED = 0,      /* the same password always                   */
    AOS_AP_PASS_ROTATING,       /* a new one each time the AP comes up        */
} aos_ap_pass_mode_t;

aos_ap_pass_mode_t aos_hal_net_ap_pass_mode(void);

/* This board's default SSID, even when one has been chosen by hand. Used by
 * the portal's "back to automatic". */
const char *aos_hal_net_ap_default_ssid(void);

/* Stores the AP configuration. Survives restarts.
 *   ssid NULL or ""  -> back to automatic AmoledOS-XXXX
 *   pass NULL or ""  -> in FIXED mode, back to the factory password
 * In ROTATING mode 'pass' is ignored: the device generates the password.
 * With the AP up the change is applied immediately (the AP is bounced), which
 * drops whoever is connected: that is inherent in changing the password.
 * Returns false if the SSID or the password are not valid (WPA2 password: 8 to
 * 63 characters, or empty to go back to the factory value). */
bool aos_hal_net_ap_set_config(const char *ssid, const char *pass,
                               aos_ap_pass_mode_t mode);

typedef struct {
    char ssid[33];
    int  rssi;                              /* dBm */
    bool secure;
} aos_wifi_ap_t;

/* Scans and fills up to max entries. Returns how many it found, or -1 on
 * failure. Blocks for a couple of seconds: call it from a task that can wait. */
int aos_hal_net_scan(aos_wifi_ap_t *out, int max);

/* --------------------------------------------------------------------------
 * Network survey
 *
 * Sweeps the WiFi networks around and, if there is a connection, the LAN:
 * which addresses answer, with what MAC and which TCP ports they have open.
 *
 * As with audio and HTTP, the slow work lives here and the app asks: a /24
 * sweep is between 5 and 60 seconds, and a dynamic app has no sockets, cannot
 * create tasks and cannot block the LVGL thread.
 *
 * The HAL also WRITES THE REPORT, in NDJSON (one JSON object per line) inside
 * aos_hal_path_scans(). That has a good consequence: the full detail -BSSID,
 * channel, encryption, MAC, ports- does not pass through this API, so widening
 * what is stored touches no structure the apps know about. The app shows
 * totals; the survey is read from the portal.
 *
 * One object per line rather than a single whole JSON on purpose: it can be
 * appended to while scanning and survives losing power or hitting stop, just
 * like the WAV header that is rewritten every two seconds.
 * -------------------------------------------------------------------------- */

#define AOS_SCAN_WIFI    0x01u      /* networks around             */
#define AOS_SCAN_HOSTS   0x02u      /* which LAN addresses are live  */
#define AOS_SCAN_PORTS   0x04u      /* TCP ports of each host      */
#define AOS_SCAN_MDNS    0x08u      /* names over mDNS             */
#define AOS_SCAN_TODO    (AOS_SCAN_WIFI | AOS_SCAN_HOSTS | AOS_SCAN_PORTS | \
                          AOS_SCAN_MDNS)

typedef enum {
    AOS_SCAN_IDLE = 0,
    AOS_SCAN_PH_WIFI,
    AOS_SCAN_PH_HOSTS,
    AOS_SCAN_PH_PORTS,
    AOS_SCAN_DONE,
    AOS_SCAN_FAILED,
    /* At the END on purpose: the values above are already baked into the
     * compiled .so files, and slipping a phase into the middle would shift
     * DONE and FAILED for them. */
    AOS_SCAN_PH_MDNS,
} aos_scan_phase_t;

typedef struct {
    aos_scan_phase_t phase;
    int      wifi_found;        /* networks seen         */
    int      hosts_found;       /* addresses that are live */
    int      ports_found;       /* open ports, total     */
    int      done, total;       /* progress of the current phase */
    uint32_t elapsed_ms;
    char     path[160];         /* the report being written */
    uint32_t reserved[4];       /* headroom so already-compiled apps do not break */
} aos_scan_status_t;

/* Starts the sweep. Returns false if one is already running or there is no
 * network. It sweeps ONLY its OWN subnet, and only if the prefix is /24 or
 * smaller: without that guard this would be a scanner of the internet rather
 * than of your house. */
bool aos_hal_scan_start(uint32_t flags);
void aos_hal_scan_stop(void);
bool aos_hal_scan_status(aos_scan_status_t *out);

/* -------------------------------------------------------------------------- */
/* HTTP: a GET that does not block                                             */
/*                                                                             */
/* A dynamic app cannot do networking on its own: it has no socket symbols, it
 * cannot create tasks, and making a request from the LVGL thread would freeze
 * the screen for however many seconds it takes. So the HAL does it in a task
 * of its own and the app asks now and then from its tick.
 *
 * It accepts http:// and https://, and for the app the difference is NONE: you
 * pass the URL and that is that. TLS is a firmware detail —the handshake, the
 * certificate store and session resumption all live inside aos_http.c—, so no
 * app changes or is recompiled because of it.
 *
 * What is worth knowing when picking the scheme:
 *
 *  - A full handshake is 1.6-1.8 s measured on this board; a resumed one
 *    against the same host, 0.6. The HAL remembers the sessions of the last
 *    two hosts, so refreshing often is cheap, but an app's FIRST request
 *    always pays the whole handshake.
 *  - Over http:// credentials travel in the clear. That is acceptable on your
 *    own LAN against your own server; against the internet, https:// and
 *    nothing else.                                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    AOS_HTTP_BUSY = 0,      /* in flight */
    AOS_HTTP_DONE,          /* finished well, the body is ready */
    AOS_HTTP_FAILED,        /* failed; the reason is in aos_hal_http_status() */
} aos_http_state_t;

/* Fires the GET and returns straight away. Returns an identifier > 0, or < 0
 * if there is no network, no free slots or the URL is no good (neither http://
 * nor https://). max_bytes is the ceiling of the body that is kept (allocated
 * in PSRAM). */
int aos_hal_http_get(const char *url, int max_bytes);

/* --------------------------------------------------------------------------
 * The same machinery, with a method, headers and a body of your own.
 *
 * Needed to talk to Home Assistant's REST API, which wants a POST with
 * 'Authorization: Bearer' and a JSON body. The GET above is this same thing
 * with the last three parameters NULL, so there are not two implementations.
 *
 *   method        "GET", "POST", "PUT"... Copied verbatim into the request.
 *   headers       extra lines, already formatted, each ending in \r\n
 *                 ("Authorization: Bearer xxx\r\n"), or NULL. Copied verbatim:
 *                 the caller is responsible for them carrying no stray line
 *                 breaks.
 *   body          body to send, NUL-terminated, or NULL. It is copied: it may
 *                 live on the caller's stack.
 *   content_type  type of the body; NULL with a body present uses
 *                 "application/json".
 *
 * There are still three slots in total, with TLS as well: two simultaneous
 * handshakes were measured to leave 93 KB of internal RAM free and not to
 * fragment.
 *
 * Credentials DO travel here —Home Assistant's 'Authorization: Bearer'—, and
 * that is why the scheme matters: over http:// they go in the clear.
 * -------------------------------------------------------------------------- */
int aos_hal_http_request(const char *method, const char *url,
                         const char *headers, const char *body,
                         const char *content_type, int max_bytes);

aos_http_state_t aos_hal_http_state(int id);

/* HTTP code (200, 404...) when DONE. When FAILED, one of the AOS_HTTP_ERR_*
 * below. */
int aos_hal_http_status(int id);

#define AOS_HTTP_ERR_DNS      (-1)   /* could not resolve the name */
#define AOS_HTTP_ERR_CONNECT  (-2)   /* could not connect */
#define AOS_HTTP_ERR_SEND     (-3)
#define AOS_HTTP_ERR_RECV     (-4)   /* cut short */
#define AOS_HTTP_ERR_PROTO    (-5)   /* a response that does not look like HTTP */
#define AOS_HTTP_ERR_MEM      (-6)

/* The two TLS ones. They are separate because the remedy differs and the app
 * can say something useful on screen instead of "the network failed":
 *
 *   NO_TIME   the board has not synchronised its clock yet, so there is no way
 *             to tell whether the certificate is still valid. It resolves
 *             itself once SNTP lands: the app can retry in a few seconds. It
 *             has a code of its own because otherwise it is indistinguishable
 *             from the one below —both come out of the handshake with the same
 *             error— and would send you hunting for the problem where it is
 *             not.
 *   TLS       the server could not be verified: unknown CA, expired
 *             certificate, name mismatch, or the server does not speak TLS.
 *             Retrying fixes nothing.                                         */
#define AOS_HTTP_ERR_SIN_HORA (-7)
#define AOS_HTTP_ERR_TLS      (-8)

/* NUL-terminated body. Valid until aos_hal_http_release(). NULL if it has not
 * finished yet. */
const char *aos_hal_http_body(int id);
int         aos_hal_http_len(int id);

/* Gives the slot back. It must always be called, including while the request
 * is still in flight: in that case the task finishes and cleans up on its own.
 * After this the body pointer is no longer valid. */
void aos_hal_http_release(int id);

/* -------------------------------------------------------------------------- */
/* Firmware update (OTA)                                                       */
/*                                                                             */
/* The partition table always had two 5 MB app slots and an otadata, and the   */
/* board already boots through them; what was missing was somebody writing to  */
/* the idle one. The image arrives from outside in pieces -the portal streams  */
/* it in from a POST- and this only knows how to put those pieces down.        */
/*                                                                             */
/* Measured cost of linking this in: 5,936 B of flash code, 2,640 B of flash   */
/* data and 260 B of DIRAM. Nothing in IRAM, which matters, because IRAM is at */
/* 100% and there would have been no room.                                     */
/*                                                                             */
/* On the desktop there is nothing to write to: begin() fails and says so.     */
/* -------------------------------------------------------------------------- */

/* Opens the idle slot. total_bytes may be 0 if the size is not known yet. */
bool aos_hal_ota_begin(size_t total_bytes);

/* In order, as the bytes arrive. */
bool aos_hal_ota_write(const void *data, size_t len);

/* Closes the image, checks it and points the bootloader at it. It does NOT
 * restart: whoever called wants to answer the request first. */
bool aos_hal_ota_end(void);

/* Gives up and leaves the running image untouched. */
void aos_hal_ota_abort(void);

/* Why the last one failed, for showing in the portal. "" if there was no
 * failure. */
const char *aos_hal_ota_error(void);

/* Rollback.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE an image that has just been
 * installed boots ON TRIAL: if nobody confirms it before the next restart, the
 * bootloader goes back to the previous one on its own. That is the whole point
 * of the safety net -an image that compiles and then hangs on boot does not
 * leave the watch needing a cable-.
 *
 * pending_verify() says whether this boot is a trial one; mark_valid() is the
 * confirmation. See main.c for when it is called and why it does not wait for
 * the network. */
bool aos_hal_ota_pending_verify(void);
void aos_hal_ota_mark_valid(void);

/* Which of the two slots is running ("ota_0" / "ota_1"). It is the only way to
 * tell from outside whether an update actually took: the version string does
 * not change between two builds of the same version, but the slot alternates
 * on every install. tools/install_fw.sh compares it before and after. */
const char *aos_hal_ota_running_slot(void);

/* P4OS: back to the image in the other slot, if it is a whole one (the
 * portal's "volver a la anterior"); takes effect at the next restart. */
bool aos_hal_ota_boot_other(void);

/* P4OS: the slots' states, read through the flash task (the portal's threads
 * cannot touch the flash themselves). other_*: the idle slot's image; its
 * version is "" when the slot holds none; other_elf, the first 16 hex of
 * its ELF's sha256 (the name tools/elf_keep.sh keeps it under). */
typedef struct {
    bool     trial;             /* the running image has not been confirmed yet */
    char     other_slot[17], other_version[33], other_built[40], other_state[12], other_elf[17];
    uint32_t other_size;
    uint32_t flash_id;          /* the flash chip's JEDEC id (manufacturer, type, capacity) */
    char     slot[17], built[40];   /* the running image's slot and build date (Settings' Update) */
} aos_ota_info_t;
bool aos_hal_ota_info(aos_ota_info_t *out);

/* P4OS: how many times in a row the hang watchdog restarted the board
 * (aos_hal_p4.c); 0 after a healthy run. Settings' Diagnostics. */
int aos_hal_hang_restarts(void);

/* P4OS: the core dump a panic left in its flash partition (espcoredump,
 * ELF). info(): present false when there is none; valid false when its
 * checksum fails. The registers are the crashing task's at the exception
 * (RISC-V has no backtrace in the summary: tools/coredump.sh decodes the
 * whole dump with the firmware's ELF). read() gives the raw ELF by pieces,
 * 0 at the end. erase() forgets it. */
typedef struct {
    bool     present, valid;
    size_t   size;
    char     task[16];
    uint32_t pc, ra, sp, mcause, mtval;
    char     elf_sha[17];       /* the crashing firmware's ELF sha, first 16 hex */
} aos_coredump_info_t;
bool   aos_hal_coredump_info(aos_coredump_info_t *out);
size_t aos_hal_coredump_read(size_t off, void *buf, size_t len);
bool   aos_hal_coredump_erase(void);

/* -------------------------------------------------------------------------- */
/* Miscellaneous                                                               */
/* -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * USB (branch usb, docs/USB.md)
 *
 * The ESP32-S3 has one USB PHY shared by the console (USB-Serial-JTAG) and
 * the OTG controller, so the port is one thing at a time. CONSOLE is what it
 * is at every boot. KEYS makes the watch a keyboard and media controller of
 * the computer it is plugged into, with the console on a serial port beside
 * it. DISK hands the microSD to the computer as a removable drive: while it
 * has it the watch has no card, and ejecting it on the computer gives it
 * back. HOST is the other way round, a pendrive plugged into the watch, and
 * needs 5 V from elsewhere (parked, see the document).
 *
 * The switch is asynchronous: aos_hal_usb_mode_set() returns at once and
 * aos_hal_usb_busy() says true until the mode has changed (a second or two
 * out of HOST, less otherwise). The simulator switches instantly and its
 * keyboard is always "ready" in KEYS mode, so the screens can be drawn.
 * -------------------------------------------------------------------------- */

typedef enum {
    AOS_HAL_USB_CONSOLE = 0,
    AOS_HAL_USB_KEYS,
    AOS_HAL_USB_DISK,
    AOS_HAL_USB_HOST,
} aos_hal_usb_mode_t;

aos_hal_usb_mode_t aos_hal_usb_mode(void);
bool aos_hal_usb_mode_set(aos_hal_usb_mode_t mode);   /* false = busy with a previous switch */
bool aos_hal_usb_busy(void);
bool aos_hal_usb_keys_ready(void);      /* KEYS mode and the computer has taken the keyboard */
bool aos_hal_usb_key(const char *name); /* "play", "next", "volup", "mute", "pgdn", "b", "esc",
                                         * "cmd+tab", "ctrl+shift+t"...: press and release. Blocks
                                         * ~50 ms; false when the keyboard is not ready. */
int  aos_hal_usb_type(const char *ascii);   /* types a string as a US keyboard; chars sent */
bool aos_hal_usb_mouse(int dx, int dy, int wheel);   /* moves the computer's pointer; clamped to +-127; no waiting */
bool aos_hal_usb_click(int button);         /* 1 left, 2 right; blocks ~40 ms */
bool aos_hal_usb_gamepad(int x, int y, int hat, unsigned buttons);   /* sticks -127..127, hat 0..8, TinyUSB's button bits */
bool aos_hal_usb_midi_ready(void);          /* KEYS mode and the computer took the MIDI port */
bool aos_hal_usb_midi_note(int note, int velocity, bool on);   /* 0..127; channel 1 */
bool aos_hal_usb_midi_cc(int control, int value);              /* 0..127 */
bool aos_hal_usb_midi_bend(int value);                         /* -8192..8191 */
bool aos_hal_usb_card_away(void);       /* DISK mode and the computer holds the card */

/* ---- USB, P4OS additions (the Macro pad; aos_usb_p4.c, sim/usb_sim.c) ----
 * On the P4 the port is the USB 2.0 High-Speed OTG (connector "OTG"); the
 * console lives on the CH343 UART, so KEYS mode costs the console nothing.
 * CONSOLE (the port idle), KEYS (a HID with a keyboard, consumer control
 * -media keys-, a mouse and a gamepad; a MIDI port; and a network over the
 * cable, the board at 192.168.7.1) and DISK (the microSD as a USB drive)
 * exist there; no HOST (no 5 V on the port). In DISK
 * the card leaves the board until the computer ejects it, and then the port
 * goes back by itself to the mode it had before (aos_usb_p4.c). Key names take any number of modifiers (cmd/win/gui,
 * ctrl, alt/opt, shift) and one key: a letter, a digit, a US-layout
 * character, f1-f24, a named key (enter, esc, tab, space, delete, fwddelete,
 * up, pgdn, home, printscreen...) or a media key (play, next, prev, mute,
 * volup, voldown, brightup, brightdown, eject). */
bool aos_hal_usb_key_valid(const char *name);  /* would aos_hal_usb_key() know this name? */
bool aos_hal_usb_connected(void);              /* KEYS or DISK, and a computer has configured the port */
bool aos_hal_usb_net_up(void);                 /* KEYS, and the network over the cable is up (192.168.7.1) */
void aos_hal_usb_restore(void);                /* at boot, once the card is read: the mode it had before
                                                * the restart (KEYS or idle; never DISK) */
bool aos_hal_usb_mouse_hold(int buttons);      /* buttons held down for the next moves (bit 0 left,
                                                * bit 1 right; 0 lets go): drag and drop */

/* P4OS: the pendrive host (aos_usb_p4.c, docs/USB.md), a switch of its own
 * beside the OTG connector's role: pendrives on the 40-pin header, with 5 V
 * from pin 1 and GND from pin 5, on one of two ports: pins 21 (D-) and 23
 * (D+), the P4's Full-Speed controller (the default), which works at the
 * same time as any role of the OTG connector; or pins 25 (D-) and 27 (D+),
 * the High-Speed one, the OTG connector's own lines, which takes the OTG
 * controller (aos_hal_usb_mode() says AOS_HAL_USB_HOST then). Behind a hub
 * there can be AOS_USB_HOST_MAX of them: their FAT volumes are mounted at
 * /usb, /usb2 and /usb3. */
#define AOS_USB_HOST_MAX 3
enum { AOS_HAL_USB_HOST_OTG = 0, AOS_HAL_USB_HOST_HEADER = 1 };
typedef struct {
    bool     device;            /* a mass storage device answered */
    bool     mounted;           /* its FAT is at path */
    uint16_t vid, pid;
    char     vendor[32], product[48];
    uint64_t bytes;             /* its capacity */
    char     error[64];         /* why it is not mounted, "" if it is */
    char     path[8];           /* "/usb", "/usb2", "/usb3"; "" if not mounted */
} aos_usb_host_info_t;
bool aos_hal_usb_host_on(void);
bool aos_hal_usb_host_set(bool on);             /* remembered across restarts; false = busy */
int  aos_hal_usb_host_port(void);               /* AOS_HAL_USB_HOST_HEADER (21/23) or _OTG (25/27) */
bool aos_hal_usb_host_port_set(int port);       /* remembered; the host, if on, moves there */
/* the pendrives there are, in slot order: -1 with the host off */
int  aos_hal_usb_host_devices(aos_usb_host_info_t *out, int max);
bool aos_hal_usb_host_info(aos_usb_host_info_t *out);   /* the first one ("device" false: none); false: host off */
const char *aos_hal_path_usb(void);                     /* "/usb" */

/* mDNS on a network interface of somebody else's (the USB one): the watch
 * answers "amoledos.local" there too, with that interface's address. The
 * argument is an esp_netif_t*, kept opaque so this header stays free of
 * ESP-IDF. Brings the responder up if WiFi never did. */
/* The watch's name: what answers as <name>.local, what the portal shows in
 * its header and the browser's tab, so two watches on one network can be
 * told apart. 1 to AOS_DEVICE_NAME_MAX characters of [a-z0-9-] (a hostname,
 * so no spaces or accents), kept in NVS, default "p4os". Setting it
 * re-announces mDNS at once; the BLE name and the USB strings do not follow
 * (the advertising packet is full and the USB descriptors are fixed at boot). */
#define AOS_DEVICE_NAME_MAX 24
const char *aos_hal_device_name(void);
bool aos_hal_device_name_valid(const char *name);
bool aos_hal_device_name_set(const char *name);

bool aos_hal_mdns_add_netif(void *esp_netif);
void aos_hal_mdns_remove_netif(void *esp_netif);

uint64_t aos_hal_uptime_ms(void);
void     aos_hal_heap_info(uint32_t *free_internal, uint32_t *free_psram);
const char *aos_hal_board_name(void);       /* "CO5300 + CST816 (v2)" etc */
const char *aos_hal_firmware_version(void);

void aos_hal_log(const char *tag, const char *fmt, ...);
/* P4OS: the log's level from now on: 1 errors, 2 warnings, 3 info (the
 * most this build compiles in). Settings, Developer keeps it. */
void aos_hal_log_level_set(int level);

/* P4OS: the log ring (aos_logring.c) - every aos_hal_log line and, on the
 * board, every ESP_LOG line. Positions are bytes since boot: read from the
 * last 'next' you got and you never see a line twice. from older than the
 * ring starts at the oldest byte kept. Returns the bytes copied (out is
 * NUL-terminated). */
size_t aos_hal_log_total(void);
/* P4OS: the tail of the previous boot's log (up to 64 KB), kept across a
 * restart - a panic or a watchdog included, not a power cut - in PSRAM that
 * the boot does not clear. Same use as aos_hal_log_read(); empty when there
 * is none. */
size_t aos_hal_log_prev_read(size_t from, char *out, size_t out_len, size_t *next);
size_t aos_hal_log_read(size_t from, char *out, size_t out_len, size_t *next);
void   aos_logring_add(const char *text, size_t len);    /* for the HALs */

/* Initialises whatever the HAL needs. Called by each platform's startup. */
bool aos_hal_init(void);

/* --------------------------------------------------------------------------
 * P4OS: threads and locks for system services
 *
 * A service (the serial capture, later Home Assistant's link) is not an app
 * and has no worker: it gets a plain task. The function may return; the
 * platform cleans up after it. 'prio' is 1 (lowest) .. 10.
 * -------------------------------------------------------------------------- */
bool  aos_hal_thread_start(const char *name, void (*fn)(void *arg), void *arg,
                           uint32_t stack_bytes, int prio);
void  aos_hal_sleep_ms(uint32_t ms);
void *aos_hal_mutex_create(void);
void  aos_hal_mutex_lock(void *m);
void  aos_hal_mutex_unlock(void *m);

/* Lock for the LVGL context. On the board esp_lvgl_port handles it (LVGL is
 * not thread-safe); in the simulator they are no-ops. Anything touching LVGL
 * objects from outside the LVGL task has to go between lock and unlock. */
bool aos_hal_lock(uint32_t timeout_ms);
int  aos_hal_lvgl_core(void);           /* the core LVGL and the panel's SPI are pinned to (-1: none) */
void aos_hal_unlock(void);

/* --------------------------------------------------------------------------
 * Worker: one background task for an app
 *
 * Everything an app does runs from LVGL's task, and until the Video app that
 * was enough. Video is the first app whose work is bigger than a frame: reading
 * and decoding a JPEG is 50 ms on the board, and doing it in the LVGL task made
 * the touch, the back swipe and the portal's capture wait behind it. This is
 * the way out, and it is deliberately small: ONE task per app, no queues, no
 * semaphores handed out. The app's function loops on its own, polls
 * aos_hal_worker_should_stop() and returns; the handshake with the UI side is
 * whatever the app builds on top of plain flags in its own memory.
 *
 * Rules, all of them measured or bitten:
 *   - The function must not touch LVGL. Not one call. It runs on the other
 *     core and LVGL has one lock, held by the task that draws.
 *   - It must return promptly once should_stop() says so: stop() waits for it
 *     (a couple of seconds at most, then the HAL logs and gives up on it).
 *   - Call stop() from destroy(). An app that exits with its worker running
 *     leaves a task reading a buffer that is about to be freed.
 *   - The stack is internal RAM (a task that reads the card goes through the
 *     filesystem, which may reach the flash driver; see AOS_XTASKCREATE).
 *     8 KB is enough for the video decoder; do not ask for more than needed.
 *     P4OS: the stack is in PSRAM (the preferences reach the flash through
 *     aos_flash_call, docs/MEMORY.md), and the defaults are core 0 - LVGL is
 *     pinned to core 1 - at priority 3, level with the apps' threads: a
 *     busy worker shares the core with the portal instead of starving it.
 *   - It is pinned to the second core, at the player's priority, one above
 *     LVGL's. (aos_hal_worker_start_on() chooses. On core 0, a worker busy
 *     nearly all the time must stay below LVGL's 4: app_main runs there at
 *     1 holding the LVGL lock, and starving it froze the Cameras app's UI
 *     for seconds, APP-GUIDE section 14.) Below LVGL's it read the card four times slower (the measure
 *     is next to AOS_WORKER_PRIO); LVGL floats between the cores and keeps
 *     the first one while the worker is busy, so the UI does not feel it.
 * -------------------------------------------------------------------------- */

typedef void (*aos_worker_fn_t)(void *arg);

/* Starts the task. false if one is already running or if there is no memory
 * for the stack. In the simulator it is a pthread. */
bool aos_hal_worker_start(const char *name, aos_worker_fn_t fn, void *arg,
                          uint32_t stack_bytes);

/* The same, on a chosen core and priority (v0.4.10). core -1 and prio -1
 * keep the defaults above (core 1, priority 5).
 *
 * Why: since v0.4.4 LVGL is pinned to core 1 too (HANDOFF-SPI-WIFI-NUCLEOS).
 * A worker there at priority 5 keeps LVGL's task off the CPU while it works,
 * so an app that renders in the worker and pushes the frame from an LVGL
 * timer gets the two in series. Turbo measured it: 40 ms of render and a
 * 16.5 ms blit made a frame every 62 ms (16 fps). On core 0, beside WiFi and
 * BT, which are idle most of the time, the render overlaps the blit.
 * The worker must still not touch LVGL nor the SPI: that is what keeps it
 * clear of esp-idf#18527. */
bool aos_hal_worker_start_on(const char *name, aos_worker_fn_t fn, void *arg,
                             uint32_t stack_bytes, int core, int prio);

/* Asks the function to return and waits for it. Safe to call with no worker. */
void aos_hal_worker_stop(void);

bool aos_hal_worker_running(void);

/* For the function: true once stop() was called. */
bool aos_hal_worker_should_stop(void);

/* For the function: sleeps without burning the core. */
void aos_hal_worker_sleep(uint32_t ms);

/* P4OS: one job in two halves, one on each core. Runs fn(arg, 1) on a
 * helper task pinned to the core the caller is NOT on and fn(arg, 0) on the
 * caller, and returns once both halves are done. The helper runs at the
 * caller's priority, so on LVGL's core (LVGL is at 4, a worker at 3) it only
 * takes what LVGL leaves. The two halves share nothing but what fn makes them
 * share; the helper's stack is 16 KB in PSRAM. Only from a worker (or a
 * thread of the app), never from the LVGL thread: it waits for the helper.
 * false: nothing ran (the simulator, or no memory for the helper) - the
 * caller does both halves itself. Measured with the 3D viewer, 103 000
 * triangles at 180x320: the triangle pass from 87 to 49 ms (1.75x), the
 * vertex pass, bound by PSRAM, not at all (17 ms); the frame 105 -> 67 ms. */
bool aos_hal_worker_split(void (*fn)(void *arg, int part), void *arg);

/* --------------------------------------------------------------------------
 * Direct blit to the panel
 *
 * Pushes a rectangle of RGB565 pixels, BIG-ENDIAN (the panel's byte order,
 * what esp_new_jpeg's JPEG_PIXEL_FORMAT_RGB565_BE produces), straight to the
 * panel over the same QSPI the LVGL port flushes through, skipping LVGL's
 * render. Measured with the Video app: rendering a full-screen canvas
 * through LVGL cost about 95 ms a frame, which capped playback at 10 fps
 * with the decoder idle a third of the time; the push alone is 16.5 ms.
 *
 * Call it with the LVGL lock held, from a timer or an event, never from a
 * worker: it shares the SPI device with LVGL's flush, and the panic of
 * v0.3.9 was exactly two callers on that device. It returns once the
 * transfer is queued, so the buffer must stay untouched until the next call
 * (the Video app keeps the frame on screen in its own slot for that reason).
 * LVGL knows nothing about what was pushed: whatever it draws next over
 * that area wins, so an app that wants labels on top invalidates them after
 * each call. In the simulator there is no panel: it returns false, and the
 * app draws through LVGL instead.
 * -------------------------------------------------------------------------- */
bool aos_hal_display_blit(int x, int y, int w, int h, const void *rgb565_be);

/* P4OS: the same, scaled by a whole number and in either byte order.
 *
 * (x, y) is where the scaled picture goes, in the screen's current
 * orientation, and it must fit: w*scale by h*scale. The HAL turns it for the
 * panel (the P4's framebuffer is portrait), so an app never rotates. On the
 * board the PPA does scale, rotation and byte order in one pass straight into
 * the framebuffer (bilinear: soft on pixel art); with the preference
 * "blit_hw" = 0, or if the PPA refuses, the CPU does it (nearest neighbour).
 * big_endian: the source is RGB565 big-endian (the watch's panel order, and
 * esp_new_jpeg's RGB565_BE); false: little-endian, LVGL's own.
 *
 * On the P4 aos_hal_display_blit() is this with scale 1 and big_endian
 * true, so a watch app works unchanged. Same rules as above: LVGL lock held,
 * never from a worker; LVGL's next redraw over the area wins. It returns
 * when the picture is in the framebuffer, so the buffer is free again. The
 * simulator returns false: draw through LVGL there. */
bool aos_hal_display_blit_scaled(int x, int y, int w, int h, const void *rgb565,
                                 int scale, bool big_endian);

/* P4OS: the same, to any size: the src_w x src_h picture (rows stride_px
 * pixels apart) fills the dst_w x dst_h rectangle at (x, y). The PPA scales
 * in steps of 1/16, so a size between those steps comes out up to 1/16
 * short, never over the rectangle: 1280x720 to 720x405 is exact (9/16). The
 * CPU fallback samples the nearest pixel. Same rules as the two above. */
bool aos_hal_display_blit_fit(int x, int y, int dst_w, int dst_h,
                              const void *rgb565, int src_w, int src_h, int stride_px,
                              bool big_endian);

/* P4OS: a rectangle already in the PANEL's own orientation (portrait,
 * AOS_PANEL_W x AOS_PANEL_H, x and y in the panel's pixels whatever the
 * screen's rotation), RGB565 little-endian, copied 1:1 by the DMA2D. For a
 * landscape app that writes its frame turned itself: the PPA turning a
 * whole 1280x720 frame took 62 ms on the board, the DMA2D copies 720x1280
 * in ~20. With the rotation at 90 the screen's pixel (x, y) is the panel's
 * (AOS_PANEL_W - 1 - y, x); at 270, (y, AOS_PANEL_H - 1 - x)
 * (aos_hal_display_get_rotation()). Same rules as the blits above; false
 * in the simulator.
 * Measured with Monster Hop lying down (2026-09-29): turning the frame on the
 * CPU while drawing it cost ~42 ms a core (PSRAM: half reading the strips,
 * half writing), more than the PPA's 62 ms running beside the render, so
 * the game kept the PPA. Worth it when the frame comes turned for free. */
bool aos_hal_display_blit_native(int x, int y, int w, int h, const void *rgb565);

/* P4OS: page flipping. The panel has three frame buffers in PSRAM: the one
 * it shows, one waiting for the next refresh (30 Hz), and one free.
 * aos_hal_display_back() gives the free one: AOS_PANEL_W x AOS_PANEL_H,
 * RGB565 little-endian, in the PANEL's orientation (see blit_native for
 * the mapping when the screen is turned), with whatever it held last. The
 * app draws a whole frame into it and aos_hal_display_flip() makes it the
 * one shown from the next refresh on: no copy, no tearing, and the caller
 * never waits for the refresh. The next back() is another buffer. From the
 * worker or the LVGL thread. As with the blits, nothing flips while
 * LVGL's panels are over the app: LVGL draws into the buffer last flipped
 * to, and the next flip would hide what it drew. NULL / false: not
 * available (the simulator, or pref "fbs" = 1). */
uint16_t *aos_hal_display_back(void);
bool      aos_hal_display_flip(uint16_t *buf);
/* For an app that redraws only what changed: how old the picture in 'fb'
 * (from back()) is. flips_since: flips since fb itself was flipped to, so
 * the app redraws the union of the areas it changed in those frames;
 * UINT32_MAX when fb has never held one of its frames. lvgl_touched: LVGL
 * drew into fb since (a panel, the switcher, a toast): redraw it all. */
bool      aos_hal_display_back_age(const uint16_t *fb, uint32_t *flips_since, bool *lvgl_touched);

/* P4OS: a rectangle of the SCREEN (its own coordinates, turned as it is)
 * into 'fb', a buffer from aos_hal_display_back(), 1:1 in LVGL's byte
 * order: the PPA turns it on the way when the screen is turned. For a
 * landscape app drawing in bands of internal RAM: each band goes out turned
 * while the cores draw the next, with no frame of its own in PSRAM.
 * Blocking; safe from both cores at once. */
bool aos_hal_display_blit_into(uint16_t *fb, int x, int y, int w, int h, const void *rgb565);
/* The same as aos_hal_display_blit_fit(), into 'fb' (from
 * aos_hal_display_back()): a small picture scaled up and turned into the
 * free buffer, then flipped to. Doom's 320x200 x3, Video's frames. */
bool aos_hal_display_blit_into_fit(uint16_t *fb, int x, int y, int dst_w, int dst_h,
                                   const void *rgb565, int src_w, int src_h, int stride_px, bool big_endian);

/* P4OS: what the panel is showing, read from the framebuffer: the panel's
 * native portrait, AOS_PANEL_W by AOS_PANEL_H, RGB565 little-endian, not
 * turned. Unlike a snapshot of LVGL it includes what apps blitted. For the
 * portal's capture; read-only. false where there is no framebuffer. */
bool aos_hal_display_fb(const uint16_t **px, int *w, int *h);

/* ==========================================================================
 * Retro canvas scaler (P4OS; board: aos_retro_p4.c, simulator: sim/retro_sim.c)
 *
 * The hardware half of aos_retro (components/aos_ui/aos_retro.c,
 * docs/RETRO.md). Apps use aos_retro_*, not these.
 *
 *   retro_alloc  a canvas buffer the scaler can read by DMA: PSRAM, aligned
 *                and padded to the 128-byte cache line, zeroed. NULL = none.
 *   retro_scale  integer upscale by 'k' of the block (in_x, in_y, in_w,
 *                in_h) of 'src' (src_w x src_h, RGB565) into 'dst' (an LVGL
 *                draw buffer of dst_bytes, dst_stride_px pixels per row,
 *                dst_h rows) at (out_x, out_y). The whole block, k*in_w by
 *                k*in_h, must fit in dst. false = not done (no PPA, buffer not
 *                aligned for it, turned off with the "retro_hw" preference):
 *                the caller scales it on the CPU. Called with LVGL's lock,
 *                from its render.
 *   uptime_us    a microsecond clock, for measuring.
 * ========================================================================== */
void    *aos_hal_retro_alloc(size_t bytes);
void     aos_hal_retro_free(void *p);
bool     aos_hal_retro_scale(const uint16_t *src, int src_w, int src_h,
                             int in_x, int in_y, int in_w, int in_h, int k,
                             uint16_t *dst, size_t dst_bytes, int dst_stride_px, int dst_h,
                             int out_x, int out_y);
uint64_t aos_hal_uptime_us(void);
/* ======================== end of the retro canvas ======================== */

/* --------------------------------------------------------------------------
 * TCP stream (aos_tcp.c, both HALs; branch rtsp)
 *
 * A plain socket for an app's WORKER: the camera viewer keeps one RTSP
 * connection open for as long as it runs, which aos_hal_http_* (one request,
 * one body) cannot do. Every call has a timeout so a worker can keep polling
 * aos_hal_worker_should_stop(); none of them may be called from LVGL's task.
 * No TLS. Four handles in the whole system: close what you open, also when
 * the worker is told to stop.
 *
 *   connect  handle > 0, or one of AOS_TCP_ERR_*. host is a name or an IP.
 *   send     sends everything or fails: len, or AOS_TCP_ERR_*.
 *   recv     waits up to timeout_ms for data: bytes read (> 0), 0 if nothing
 *            came in time, AOS_TCP_ERR_CLOSED when the peer closed or reset.
 * -------------------------------------------------------------------------- */
#define AOS_TCP_ERR_ARG      (-1)
#define AOS_TCP_ERR_SLOTS    (-2)
#define AOS_TCP_ERR_DNS      (-3)
#define AOS_TCP_ERR_CONNECT  (-4)
#define AOS_TCP_ERR_TIMEOUT  (-5)
#define AOS_TCP_ERR_CLOSED   (-6)

int  aos_hal_tcp_connect(const char *host, int port, int timeout_ms);
int  aos_hal_tcp_send(int handle, const void *data, int len, int timeout_ms);
int  aos_hal_tcp_recv(int handle, void *buf, int max, int timeout_ms);
void aos_hal_tcp_close(int handle);

/* --------------------------------------------------------------------------
 * WebSocket client (aos_ws.c, both HALs; P4OS, for Home Assistant)
 *
 * For a service's thread, never from LVGL's task; every call has a timeout.
 * ws:// and wss:// (TLS from aos_tls.c, the same trust store and session
 * cache as aos_hal_http_*; a full handshake is 1.6-1.8 s on the board, and
 * the calling thread needs ~7 KB of stack for the certificate check). Its
 * connection is not one of the aos_hal_tcp_* slots.
 *
 *   open       url "ws[s]://host[:port][/path]" (default port 80 / 443);
 *              extra_headers, if any, are whole "Name: value\r\n" lines.
 *              NULL if it could not connect, the certificate was refused (the
 *              reason is logged under "ws") or the server refused the upgrade.
 *   send_text  0, or -1 if the connection is gone. len -1 = strlen.
 *   recv_text  one whole text message: its length (> 0) with *out pointing
 *              at it, NUL-terminated and valid until the next recv; 0 if none
 *              came in timeout_ms; -1 when the connection is gone. Server
 *              pings are answered inside.
 * -------------------------------------------------------------------------- */
typedef struct aos_ws aos_ws_t;
aos_ws_t *aos_hal_ws_open(const char *url, const char *extra_headers, int timeout_ms);
int       aos_hal_ws_send_text(aos_ws_t *ws, const char *text, int len);
int       aos_hal_ws_recv_text(aos_ws_t *ws, const char **out, int timeout_ms);
bool      aos_hal_ws_is_open(aos_ws_t *ws);
void      aos_hal_ws_close(aos_ws_t *ws);
/* Why the last aos_hal_ws_open() failed ("certificate for x is not signed by
 * a trusted authority"...), "" if it did not or there is no reason. */
const char *aos_hal_ws_last_error(void);

/* --------------------------------------------------------------------------
 * Pictures (P4OS; aos_image_p4.c on the board, libavcodec in the simulator)
 *
 * Decodes the picture in 'path' - JPEG, PNG or BMP - all of the file with
 * size == 0, else 'size' bytes from 'offset' (the cover inside an MP3).
 * RGB565 in LVGL's byte order, allocated by the HAL and freed with
 * aos_hal_image_free().
 *
 *   fill = false  scaled to fit inside max_w x max_h, aspect kept, never
 *                 enlarged;
 *   fill = true   scaled to cover max_w x max_h and cropped to its centre:
 *                 the result is exactly that size (enlarged if need be).
 *
 * *out_w, *out_h get the size of the result, and *src_w, *src_h (may be
 * NULL) the picture's own. NULL if it is not a picture it can read
 * (progressive JPEGs included, on the board). Slow by LVGL's measure: call
 * it from a thread of your own.
 * -------------------------------------------------------------------------- */
uint16_t *aos_hal_image_decode(const char *path, uint32_t offset, uint32_t size,
                               int max_w, int max_h, bool fill,
                               int *out_w, int *out_h, int *src_w, int *src_h);
void      aos_hal_image_free(void *px);

/* MD5 of a buffer as 32 lower-case hex digits: RTSP's Digest authentication. */
void aos_hal_md5_hex(const void *data, size_t len, char out[33]);

/* --------------------------------------------------------------------------
 * H.264 decoder (branch rtsp)
 *
 * The board has no video block: this is Espressif's software decoder
 * (esp_h264 = tinyh264), which speaks CONSTRAINED BASELINE only - CAVLC, no
 * B-frames. A Main or High stream with CABAC does not decode at all (every
 * slice is an error, not a slow picture). Measured on this board, one task,
 * decoder in IRAM (docs/CAMERAS.md): 704x576 at 17 fps (P 54 ms, I 218 ms),
 * 640x480 at 24, 640x360 at 30, 1280x720 at 8.6 with a 105 KB internal RAM
 * peak. The picture buffers (~1.5 MB at 704x576) go to PSRAM. In the Cameras
 * app, with the network and the panel running, the same stream costs
 * P 70 ms and I 225 ms: the two cores share 16 KB of instruction cache and
 * 32 KB of data cache.
 *
 * In the simulator it is libavcodec, which decodes anything; the board's
 * limits are the board's.
 *
 * decode() takes ONE NAL unit WITH its Annex-B start code (00 00 01 or
 * 00 00 00 01), in stream order, parameter sets included. It returns 1 and
 * fills *pic when a picture came out, 0 when the NAL was taken with nothing
 * to show yet (SPS, PPS, a slice of a picture not finished), or < 0 when the
 * NAL was refused. After an error, feed on: the decoder resynchronises at the
 * next keyframe.
 *
 * How long a picture's planes stay put: they are the decoder's own buffers.
 * A picture is the reference of the next one, so decoding the next P frame
 * does not write them; decoding the one after that, or an IDR, may. The
 * Cameras app converts picture N on the other core while N+1 decodes, and
 * holds the decoder before N+2 or an IDR until it is done
 * (apps/camaras/main/cam_view.h).
 *
 * Call it from a worker, never from LVGL's task: a keyframe is a fifth of a
 * second.
 * -------------------------------------------------------------------------- */
typedef struct aos_h264 aos_h264_t;

/* decode() < 0: -1 the NAL was refused; AOS_H264_ERR_MEM the picture buffers
 * for this size do not fit (a 1920x1080 stream on this board). */
#define AOS_H264_ERR_MEM (-2)

typedef struct {
    const uint8_t *y, *u, *v;       /* I420 planes */
    int width, height;              /* luma, in pixels (tinyh264 does not apply the
                                       SPS crop: 640x360 comes out 640x368) */
    int stride_y, stride_uv;        /* bytes per row of each plane */
} aos_h264_pic_t;

aos_h264_t *aos_hal_h264_open(void);
int         aos_hal_h264_decode(aos_h264_t *dec, const uint8_t *nal, int len, aos_h264_pic_t *pic);
void        aos_hal_h264_close(aos_h264_t *dec);

/* --------------------------------------------------------------------------
 * P4OS Cameras: JPEG frames in memory (aos_jpeg_p4.c on the board,
 * sim/jpeg_sim.c in the simulator)
 *
 * aos_hal_image_decode() reads a file and allocates per call; a camera hands
 * over 10-25 JPEGs a second, from memory. open() keeps a decoder and its
 * output buffer for the life of the stream; on the board it is the P4's JPEG
 * engine (baseline, the output padded to 16 px), with esp_new_jpeg in
 * software for what the engine refuses (progressive, odd sampling).
 *
 *   decode  one whole JPEG (SOI..EOI) -> RGB565 in LVGL's byte order, at
 *           the picture's own size. The pixels are the handle's: valid until
 *           the next decode() or close(). *stride_px is the row length (>= w).
 *           NULL if it is not a JPEG it can read.
 *   save    encodes w x h RGB565 (LVGL order) to a baseline JPEG file,
 *           quality 1-100; creates the folder if it is missing. In software
 *           on both: a snapshot, not a stream.
 *
 * From a thread of the app's own, never from LVGL's task.
 * -------------------------------------------------------------------------- */
typedef struct aos_jpeg aos_jpeg_t;

aos_jpeg_t     *aos_hal_jpeg_open(void);
const uint16_t *aos_hal_jpeg_decode(aos_jpeg_t *j, const void *data, size_t len,
                                    int *w, int *h, int *stride_px);
void            aos_hal_jpeg_close(aos_jpeg_t *j);
bool            aos_hal_jpeg_save(const char *path, const uint16_t *px, int w, int h,
                                  int stride_px, int quality);

/* --------------------------------------------------------------------------
 * Steps: today, the last seven days, the goal (aos_steps.c, both HALs)
 *
 * The raw counter (aos_hal_imu_steps) only grows since boot. This is the
 * watch's view of it: today's steps kept across restarts, cut at midnight
 * by the clock, seven days of history, and a goal. Written to NVS every
 * five minutes and at every day change.
 * -------------------------------------------------------------------------- */
#define AOS_STEPS_DAYS 7

typedef struct {
    uint32_t today;
    uint32_t goal;
    uint32_t history[AOS_STEPS_DAYS];   /* [0] = yesterday ... [6] = a week ago */
    bool     day_known;                 /* false until the clock was set once */
} aos_steps_info_t;

bool aos_hal_steps_get(aos_steps_info_t *out);
void aos_hal_steps_set_goal(uint32_t goal);    /* 1000..50000, kept in NVS */
void aos_hal_steps_reset_today(void);

/* HAL-internal: the board's housekeeping and the simulator's loop call it
 * every few seconds; it folds the raw counter in and watches the date. */
void aos_steps_tick(void);
/* HAL-internal: saves the count now (see aos_steps.c). */
void aos_steps_flush(void);
/* HAL-internal: writes the battery history to the card now (aos_stats.c). */
void aos_stats_flush(void);

/* --------------------------------------------------------------------------
 * Link: raw ESP-NOW frames between watches (phase 1 of docs/LINK.md)
 *
 * No discovery, no pairing, no retries yet: a frame to a MAC (or to
 * everyone), what came in, and counters. The receive side runs in the
 * HAL's own task and leaves the frames in a ring the app drains from its
 * tick (aos_hal_link_recv), like the microphone. Frames are at most 250
 * bytes (ESP-NOW v1) in this phase. The radio must be on: the link rides on
 * the station interface, connected or not.
 * -------------------------------------------------------------------------- */
#define AOS_LINK_MAX_FRAME 250

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[AOS_LINK_MAX_FRAME];
} aos_link_frame_t;

typedef struct {
    bool     running;
    uint32_t version;           /* ESP-NOW version the controller supports */
    uint8_t  channel;
    uint8_t  own_mac[6];
    uint32_t sent, ack_ok, ack_fail, send_err;
    uint32_t received, dropped;
    int8_t   last_rssi;
    uint8_t  last_mac[6];
    /* the test protocol (/api/link) */
    uint32_t test_tx, test_rx, test_lost, echo_rx;
    uint32_t rtt_sum_us, rtt_min_us, rtt_max_us;
    uint32_t test_ms;           /* how long the last test took to send */
    /* the reliable channel */
    uint32_t rel_tx, rel_acked, rel_retx, rel_rx, rel_lost, rel_pending;
    bool     rel_synced;        /* the receiver knows where my numbering starts */
    /* the bulk test over it */
    uint32_t bulk_ms, bulk_rx_bytes, bulk_rx_bad, drop_count;
} aos_link_stats_t;

bool aos_hal_link_start(void);
void aos_hal_link_stop(void);
bool aos_hal_link_running(void);
bool aos_hal_link_send(const uint8_t mac[6], const void *data, size_t len);  /* NULL mac = broadcast */
int  aos_hal_link_recv(aos_link_frame_t *out);        /* bytes, 0 if none */
bool aos_hal_link_stats(aos_link_stats_t *out);
void aos_hal_link_stats_reset(void);
/* N numbered test frames to 'mac' (NULL = broadcast), 'gap_ms' apart, of
 * 'len' bytes, echoed back by the receiver when 'echo'. Runs in its own
 * task; the counters tell the story. */
bool aos_hal_link_test(const uint8_t mac[6], uint32_t n, uint32_t gap_ms, bool echo, uint16_t len);
bool aos_hal_link_test_running(void);
/* Phase 2: who is around, and the one we are paired with.
 *
 * While the link is up the watch beacons once a second (its name, the app
 * it offers); neighbours are whoever beaconed in the last five seconds.
 * Pairing is the bump: with pairing enabled, a bump on this watch and a
 * bump frame from a neighbour within 400 ms of each other, close by (RSSI),
 * make them partners: a key derived from both nonces, an encrypted peer,
 * and the partner kept in NVS so any link app finds it without pairing
 * again. One partner at a time. */
#define AOS_LINK_NAME_MAX  24
#define AOS_LINK_NEIGHBOURS 8

typedef struct {
    uint8_t  mac[6];
    char     name[AOS_LINK_NAME_MAX + 1];
    char     app[AOS_LINK_NAME_MAX + 1];    /* what it offers, "" if nothing */
    int8_t   rssi;
    uint32_t age_ms;                        /* since its last beacon */
} aos_link_neighbour_t;

typedef struct {
    bool     valid;                         /* there is a partner in NVS */
    bool     seen;                          /* it beaconed in the last 5 s */
    bool     confirmed;                     /* it answered on the encrypted peer this session */
    uint8_t  mac[6];
    char     name[AOS_LINK_NAME_MAX + 1];
    int8_t   rssi;
    uint32_t age_ms;
} aos_link_partner_t;

void aos_hal_link_offer(const char *app);          /* what the beacon says we offer */
int  aos_hal_link_neighbours(aos_link_neighbour_t *out, int max);
void aos_hal_link_pair_enable(bool on);            /* listen for bumps */
bool aos_hal_link_pairing(void);
void aos_hal_link_bump(void);                      /* a bump, from the IMU or from /api/link for tests */
bool aos_hal_link_partner(aos_link_partner_t *out);
void aos_hal_link_unpair(void);
bool aos_hal_link_send_partner(const void *data, size_t len);   /* encrypted, to the partner */
uint32_t aos_hal_link_pair_events(void);           /* how many times it paired, for the stats */

/* Phase 3: two channels to the partner.
 *   reliable: in order, acknowledged, resent (go-back-N, window 4, 80 ms);
 *             a frame is at most AOS_LINK_MAX_FRAME - 8 bytes; send() says
 *             false when the queue of 16 is full (try again next tick) or
 *             the channel is lost (no ack in 3.2 s: reset it and decide).
 *   fast:     aos_hal_link_send_partner / aos_hal_link_recv above: send and
 *             forget, the last one wins. */
bool aos_hal_link_send_reliable(const void *data, size_t len);
int  aos_hal_link_recv_reliable(aos_link_frame_t *out);   /* bytes, 0 if none */
int  aos_hal_link_reliable_pending(void);                  /* queued and in flight */
bool aos_hal_link_reliable_lost(void);
void aos_hal_link_reliable_reset(void);
/* the bulk test: N bytes over the reliable channel, checked on the other
 * side; drop_percent makes a receiver lose frames on purpose */
bool aos_hal_link_bulk_test(uint32_t bytes);
void aos_hal_link_drop_percent(uint32_t pct);
void aos_hal_link_bulk_drain(void);
void aos_hal_link_bulk_receiver(bool on);                  /* the poll drains and checks, for the test */
void aos_hal_link_set_partner_test(const uint8_t mac[6]);  /* tests only: a partner without a bump */
void aos_hal_link_set_channel_info(uint8_t channel);       /* raw layer -> stats */

/* The channel policy (LINK.md): leave the access point and sit on 'channel'
 * so two watches on different networks meet; unpark reconnects and
 * measures the time back to an address. While parked the portal is off
 * the air: whatever parks must schedule its own unpark. */
bool     aos_hal_link_park(uint8_t channel);
void     aos_hal_link_unpark(void);
bool     aos_hal_link_parked(void);
uint32_t aos_hal_link_rejoin_ms(void);

/* --------------------------------------------------------------------------
 * FTM: distance by time of flight (IEEE 802.11mc), for the radar
 *
 * One watch answers (the internal softAP brought up with the FTM responder
 * flag, on the station's channel so the portal stays up) and the other asks
 * (a session of N frames against that BSSID; the driver averages them and
 * gives a round trip in ns and a distance in cm). Asynchronous: measure()
 * returns at once and result() says when the report came. Only on the board
 * with CONFIG_ESP_WIFI_FTM_ENABLE; elsewhere supported() is false.
 * -------------------------------------------------------------------------- */
typedef struct {
    bool     busy;              /* a session is in the air */
    bool     valid;             /* the numbers below come from a session that succeeded */
    uint8_t  status;            /* 0 ok, else the driver's wifi_ftm_status_t */
    uint32_t rtt_ns;            /* estimated round trip */
    uint32_t dist_cm;           /* estimated one-way distance */
    uint32_t sessions, failures;
    uint32_t ms;                /* uptime of the last report */
} aos_ftm_result_t;

bool aos_hal_ftm_supported(void);
bool aos_hal_ftm_responder(bool on);                          /* softAP up/down as responder */
bool aos_hal_ftm_responder_info(uint8_t mac[6], uint8_t *channel); /* what the initiator needs */
bool aos_hal_ftm_measure(const uint8_t mac[6], uint8_t channel, uint8_t frames);
bool aos_hal_ftm_result(aos_ftm_result_t *out);

/* --------------------------------------------------------------------------
 * P4OS: network tools (the Red app, aos.net)                    BEGIN aos.net
 *
 * What aos_hal_net_scan() leaves out and an analyser needs: the channel, the
 * BSSID, the kind of security, the width. A new struct and a new call rather
 * than a wider aos_wifi_ap_t, whose size is baked into compiled .so files.
 * Unlike aos_hal_net_scan() it keeps EVERY access point (one network with
 * three APs is three bells on the channel graph). Blocking: from a thread.
 * -------------------------------------------------------------------------- */
typedef enum {
    AOS_WIFI_AUTH_OPEN = 0,
    AOS_WIFI_AUTH_WEP,
    AOS_WIFI_AUTH_WPA,
    AOS_WIFI_AUTH_WPA2,
    AOS_WIFI_AUTH_WPA_WPA2,
    AOS_WIFI_AUTH_WPA3,
    AOS_WIFI_AUTH_WPA2_WPA3,
    AOS_WIFI_AUTH_ENTERPRISE,
    AOS_WIFI_AUTH_OTHER,
} aos_wifi_auth_t;

typedef struct {
    char    ssid[33];               /* "" for a hidden network */
    uint8_t bssid[6];
    int8_t  rssi;                   /* dBm */
    uint8_t channel;                /* primary: 1..14 is 2,4 GHz, 32.. is 5 GHz */
    uint8_t width;                  /* MHz: 20, 40, 80, 160 */
    int8_t  second;                 /* 40 MHz: +1 above the primary, -1 below, 0 none */
    uint8_t auth;                   /* aos_wifi_auth_t */
    uint8_t phy;                    /* bits: 1 b, 2 g, 4 n, 8 a, 16 ac, 32 ax */
} aos_wifi_ap_ex_t;

/* Scans and fills up to max, strongest first. -1 on failure (no radio). */
int  aos_hal_net_scan_ex(aos_wifi_ap_ex_t *out, int max);

/* The access point the station is associated with, right now (RSSI fresh).
 * false when not connected. Cheap enough for a 2 Hz trace, but not free on
 * the board (an RPC to the C6): from a thread. */
bool aos_hal_net_ap_info(aos_wifi_ap_ex_t *out);

/* lwIP's ARP table as it is right now: the station's resolved entries, as
 * host-order IPv4 and MAC. It holds 10 and recycles them as new addresses are
 * asked for, so a sweep reads it after every batch it sends. Returns how many
 * it filled; 0 where there is no such table (the simulator). */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
} aos_arp_entry_t;
int  aos_hal_net_arp_table(aos_arp_entry_t *out, int max);

/* The station's own MAC; false without the radio. */
bool aos_hal_net_mac(uint8_t mac[6]);

/* mDNS browsing. 'types' are full service types, "_http._tcp"; all of them
 * are asked at once and answers are gathered for timeout_ms. One entry per
 * instance: its type, name, host, first IPv4 address (host order, 0 if the
 * answer brought none) and port, and the TXT records as "k=v;k=v". Returns
 * how many it filled, -1 if mDNS is not running. Blocking: from a thread. */
typedef struct {
    char     type[32];              /* "_esphomelib._tcp" */
    char     instance[64];
    char     host[64];              /* without ".local" */
    uint32_t ip;
    uint16_t port;
    char     txt[96];
} aos_mdns_svc_t;

int  aos_hal_mdns_browse(const char *const *types, int ntypes, uint32_t timeout_ms,
                         aos_mdns_svc_t *out, int max);
/* ------------------------------------------------------------ END aos.net */

#ifdef __cplusplus
}

#endif
