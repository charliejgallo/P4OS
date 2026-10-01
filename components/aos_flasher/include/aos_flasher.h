/*
 * P4OS - The serial programmer service: flashes ESP32-family chips (ESP32,
 * S2, S3, C2, C3, C5, C6, H2, P4, ESP8266) through a UART port of the header,
 * resetting the target into its ROM bootloader with the EN and BOOT lines of
 * the module on that port (aos_io_uart_lines). The engine is Espressif's
 * esp-serial-flasher; this is the part of it that knows about ports, the
 * card and a thread.
 *
 * A service, not an app: one job at a time runs in its own thread and goes on
 * with the Programador app closed. Everything here is callable from the LVGL
 * task - the calls only copy state under a lock or start/stop the thread.
 *
 * Firmware comes from the card, <sd>/firmware/ (aos_flasher_dir()):
 *   - a folder with ESP-IDF's flasher_args.json and its bins (what
 *     `idf.py build` leaves in build/; the folder itself or a build/ inside
 *     it): every file at its offset;
 *   - a .bin whose name ends in "@0x10000.bin": that file at that offset;
 *   - any other .bin: a merged/factory image at 0x0 - unless it is a bare
 *     application image (it has the app descriptor), which goes to 0x10000.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_FLASHER_OWNER     "Programador"     /* the owner of the port's pins */
#define AOS_FLASHER_FILES_MAX 8
#define AOS_FLASHER_SOURCES_MAX 24

/* ---- firmware on the card ---- */

typedef struct {
    uint32_t offset;
    uint32_t size;              /* bytes in the file */
    char     name[64];          /* as flasher_args.json names it ("bootloader/bootloader.bin"),
                                 * relative to the source's path; a loose .bin: its own name */
} aos_flasher_file_t;

typedef enum {
    AOS_FLASHER_PROJECT = 0,    /* flasher_args.json gives every offset */
    AOS_FLASHER_BIN_AT,         /* name@0x10000.bin */
    AOS_FLASHER_BIN_APP,        /* a bare app image: 0x10000, over the bootloader already there */
    AOS_FLASHER_BIN_MERGED,     /* anything else: a whole image from 0x0 */
} aos_flasher_kind_t;

typedef struct {
    char     name[48];          /* the folder or the file */
    char     path[256];         /* absolute: the folder with flasher_args.json, or the .bin */
    bool     project;           /* a folder with flasher_args.json */
    char     chip[16];          /* "esp32s3": from flasher_args.json or the image header; "" unknown */
    char     flash_mode[8], flash_size[8], flash_freq[8];   /* "dio" "4MB" "80m", "" unknown */
    char     app_name[32];      /* from the app descriptor, when there is one */
    char     app_version[32];
    int      nfiles;
    aos_flasher_file_t files[AOS_FLASHER_FILES_MAX];
    uint32_t total;             /* bytes of all the files */
    aos_flasher_kind_t kind;    /* how the offsets were decided */
} aos_flasher_source_t;

/* <sd>/firmware; NULL without a card. */
const char *aos_flasher_dir(void);
/* What is in it, projects first, then loose .bin files, by name. Reads the
 * card: from a worker or on a user's tap, not every frame. */
int  aos_flasher_scan(aos_flasher_source_t *out, int max);
/* One source by its path (a folder or a .bin). */
bool aos_flasher_source_load(const char *path, aos_flasher_source_t *out, char *err, size_t err_len);

/* ---- jobs ---- */

typedef enum {
    AOS_FLASHER_IDLE = 0,
    AOS_FLASHER_OPENING,        /* the port */
    AOS_FLASHER_CONNECTING,     /* reset into the ROM, SYNC */
    AOS_FLASHER_STUB,           /* uploading the flasher stub */
    AOS_FLASHER_BAUD,           /* going to the fast speed */
    AOS_FLASHER_INFO,           /* chip, MAC, flash */
    AOS_FLASHER_ERASING,        /* FLASH_BEGIN of a file */
    AOS_FLASHER_WRITING,
    AOS_FLASHER_VERIFYING,      /* MD5 of what the target has */
    AOS_FLASHER_RESETTING,
    AOS_FLASHER_DONE,
    AOS_FLASHER_FAILED,
    AOS_FLASHER_CANCELLED,
} aos_flasher_phase_t;

typedef enum { AOS_FLASHER_JOB_NONE = 0, AOS_FLASHER_JOB_DETECT, AOS_FLASHER_JOB_FLASH } aos_flasher_job_t;

typedef struct {
    uint32_t baud;              /* for the writing; 0 = 460800. Falls back to 115200 */
    bool     no_stub;           /* talk to the ROM itself (slower, no ERASE_REGION) */
    bool     no_verify;         /* skip the MD5 of each file */
    bool     no_reset;          /* leave the target in its bootloader at the end */
} aos_flasher_opts_t;

/* Connects, reads the chip and resets the target back to its program. */
bool aos_flasher_detect(const char *port);
/* Flashes 'src' (copied) through 'port'. false: a job is running, or the
 * thread could not start. */
bool aos_flasher_flash(const char *port, const aos_flasher_source_t *src, const aos_flasher_opts_t *opts);
void aos_flasher_cancel(void);
bool aos_flasher_busy(void);

typedef struct {
    bool     busy;
    aos_flasher_job_t job;
    aos_flasher_phase_t phase;
    uint32_t seq;               /* bumps whenever a job starts or ends */
    char     port[16];
    char     source[48];        /* the name of what is being flashed */
    /* progress, over the whole job */
    int      file_index, file_count;    /* 0-based */
    char     file_name[64];
    uint32_t file_offset;
    uint32_t done, total;       /* bytes */
    int      percent;           /* 0..100 */
    uint32_t bytes_per_s;       /* while writing */
    uint32_t elapsed_ms;        /* since the job started; frozen at the end */
    uint32_t eta_ms;            /* 0: unknown */
    /* the result of the last job */
    bool     ok;
    bool     verified;          /* every file checked by MD5 */
    char     error[112];
    /* the chip, once a job got that far */
    bool     chip_known;
    char     chip[16];          /* "ESP32-S3" */
    uint16_t revision;          /* major * 100 + minor, 0xFFFF unknown */
    uint8_t  mac[6];
    bool     mac_known;
    uint32_t flash_size;        /* bytes, 0 unknown */
    bool     stub;              /* the flasher stub ran */
    uint32_t baud;              /* the speed the job ended up at */
    bool     secure;            /* secure boot or flash encryption on */
    bool     lines;             /* the port has EN/BOOT */
} aos_flasher_status_t;

void aos_flasher_status(aos_flasher_status_t *out);

/* ---- the log of the jobs ----
 * A ring of lines: 'count' only grows, so a view keeps the last index it
 * showed. Line 'index' is copied to out; returns its level (0 info, 1 ok,
 * 2 warning, 3 error) or -1 if it already left the ring. */
#define AOS_FLASHER_LOG_LINE 100
uint32_t aos_flasher_log_count(void);
uint32_t aos_flasher_log_first(void);
int      aos_flasher_log_line(uint32_t index, char *out, int cap);

/* "ESP32-S3" for "esp32s3", as the service names chips. */
const char *aos_flasher_chip_label(const char *idf_name);

#ifdef __cplusplus
}
#endif
