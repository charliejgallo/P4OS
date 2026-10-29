/*
 * P4OS - The serial programmer service: flashes ESP32-family chips (ESP32,
 * S2, S3, C2, C3, C5, C6, H2, P4, ESP8266) through a UART port of the header,
 * resetting the target into its ROM bootloader with the EN and BOOT lines of
 * the module on that port (aos_io_uart_lines). The engine is Espressif's
 * esp-serial-flasher; this is the part of it that knows about ports, the
 * card and a thread.
 *
 * Each job is for one family of targets (aos_flasher_opts_t.family): the
 * ESP32s above, the AVR Arduinos through their own bootloader (Uno, Nano,
 * Pro Mini, Mega, Leonardo, Micro: aos_flasher_avr.c), from a .hex, or an
 * Arm Cortex-M through its SWD port (nRF52, RP2040, STM32F0/F1:
 * aos_flasher_swd.c), on the pins of a UART port of the header: TX is
 * SWCLK, RX is SWDIO, EN is NRST. An SWD job can also be the target's RTT
 * console: what its firmware writes, read from its RAM while it runs. And
 * an STM32 through the bootloader in its system memory, on a UART with
 * BOOT0 on the port's BOOT line and NRST on EN (aos_flasher_stm32.c).
 *
 * A service, not an app: one job at a time runs in its own thread and goes on
 * with the Programador app closed. Everything here is callable from the LVGL
 * task - the calls only copy state under a lock or start/stop the thread.
 *
 * Besides writing, a job can read the target's flash to a file on the card
 * (a dump, for a backup or to clone a board) or erase all of it.
 *
 * Firmware comes from the card, <sd>/firmware/ (aos_flasher_dir()):
 *   - a folder with ESP-IDF's flasher_args.json and its bins (what
 *     `idf.py build` leaves in build/; the folder itself or a build/ inside
 *     it): every file at its offset;
 *   - a .bin whose name ends in "@0x10000.bin": that file at that offset;
 *   - any other .bin: a merged/factory image at 0x0 - unless it is a bare
 *     application image (it has the app descriptor), which goes to 0x10000.
 * Dumps go to <sd>/firmware/volcados/ as "<chip>-<date>@0x<from>.bin", each
 * with a .json beside it, and come back in the list as firmware to write.
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

/* ---- families ---- */

typedef enum {
    AOS_FLASHER_ESP = 0,        /* ESP32 / ESP8266 through the ROM's serial bootloader */
    AOS_FLASHER_AVR,            /* an Arduino through its bootloader: STK500 v1/v2, AVR109 */
    AOS_FLASHER_SWD,            /* an Arm Cortex-M through its debug port */
    AOS_FLASHER_STM32,          /* an STM32 through its system bootloader (AN3155) */
    AOS_FLASHER_FAMILIES,
} aos_flasher_family_t;

/* "ESP32", "Arduino" */
const char *aos_flasher_family_name(int family);

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
    AOS_FLASHER_IMAGE,          /* a .hex, .elf or .uf2: it says its own addresses */
} aos_flasher_kind_t;

typedef struct {
    char     name[48];          /* the folder or the file */
    char     path[256];         /* absolute: the folder with flasher_args.json, or the .bin */
    bool     project;           /* a folder with flasher_args.json */
    char     chip[16];          /* "esp32s3": from flasher_args.json or the image header; "" unknown */
    char     flash_mode[8], flash_size[8], flash_freq[8];   /* "dio" "4MB" "80m", "" unknown */
    char     app_name[32];      /* from the app descriptor, when there is one */
    char     app_version[32];
    /* the chip revisions its images accept (major * 100 + minor), from the
     * extended header; rev_max 0: not said (an image older than IDF 5) */
    uint16_t rev_min, rev_max;
    int      nfiles;
    aos_flasher_file_t files[AOS_FLASHER_FILES_MAX];
    uint32_t total;             /* bytes of all the files */
    aos_flasher_kind_t kind;    /* how the offsets were decided */
    bool     dump;              /* read off a board by the programmer (firmware/volcados) */
    int8_t   family;            /* the family it is for (an ESP image, a dump's .json), -1 not known */
} aos_flasher_source_t;

/* Whether the list shows 'src' for a job of 'family': its own, or one that
 * does not say - except a .hex/.elf/.uf2 for the ESP family, which only
 * writes images. */
bool aos_flasher_source_fits(const aos_flasher_source_t *src, int family);

/* <sd>/firmware; NULL without a card. Besides the ESP folders and .bin
 * files, any .hex, .elf or .uf2 in it is listed (AOS_FLASHER_IMAGE: one
 * "file" from its lowest address, its size the bytes it holds). */
const char *aos_flasher_dir(void);
/* <sd>/firmware/volcados, where the dumps go; NULL without a card. */
const char *aos_flasher_dump_dir(void);
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
    AOS_FLASHER_READING,        /* a dump: the flash to the card */
    AOS_FLASHER_RESETTING,
    AOS_FLASHER_CONSOLE,        /* the RTT console: reading until cancelled */
    AOS_FLASHER_BRIDGE,         /* the port lent over TCP, until cancelled */
    AOS_FLASHER_DONE,
    AOS_FLASHER_FAILED,
    AOS_FLASHER_CANCELLED,
} aos_flasher_phase_t;

typedef enum {
    AOS_FLASHER_JOB_NONE = 0, AOS_FLASHER_JOB_DETECT, AOS_FLASHER_JOB_FLASH,
    AOS_FLASHER_JOB_READ,       /* a dump to the card */
    AOS_FLASHER_JOB_ERASE,      /* all of the flash */
    AOS_FLASHER_JOB_CONSOLE,    /* SWD: the target's RTT console, until cancelled */
    AOS_FLASHER_JOB_BRIDGE,     /* the port over TCP for the computer's tools, until cancelled */
} aos_flasher_job_t;

typedef struct {
    aos_flasher_family_t family;    /* who the target is; 0 = an ESP */
    uint32_t baud;              /* for the writing; 0 = 460800. Falls back to 115200. Not for an AVR */
    bool     no_stub;           /* talk to the ROM itself (slower, no ERASE_REGION) */
    bool     no_verify;         /* skip the MD5 of each file */
    bool     no_reset;          /* leave the target in its bootloader at the end */
    /* a dump: from this address, this many bytes (0 = to the end of the
     * flash); with no_verify each piece is not checked against the target */
    uint32_t read_from, read_len;
} aos_flasher_opts_t;

/* Connects, reads the chip and resets the target back to its program. */
bool aos_flasher_detect(const char *port);
/* The same for a family other than the ESP's (opts->family; the rest is
 * not used). */
bool aos_flasher_detect_ex(const char *port, const aos_flasher_opts_t *opts);
/* Flashes 'src' (copied) through 'port'. false: a job is running, or the
 * thread could not start. */
bool aos_flasher_flash(const char *port, const aos_flasher_source_t *src, const aos_flasher_opts_t *opts);
/* Reads the target's flash (opts->read_from/read_len) to a new file in
 * aos_flasher_dump_dir(); the status says where (out_path) when it ends. */
bool aos_flasher_read(const char *port, const aos_flasher_opts_t *opts);
/* Erases all of the target's flash: what is on it is gone. */
bool aos_flasher_erase(const char *port, const aos_flasher_opts_t *opts);
/* SWD: the target's RTT console. What its firmware writes to its first up
 * buffer goes to the log, a line at a time (level 1), until cancelled;
 * aos_flasher_console_send() puts bytes in its first down buffer. */
bool aos_flasher_console(const char *port, const aos_flasher_opts_t *opts);
bool aos_flasher_console_send(const void *data, int n);
/* The port lent to the computer over TCP (aos_flasher_bridge.c): RFC 2217
 * on 4000 (esptool, pyserial), the bytes alone on 4001 with an Arduino's
 * reset at each connection (avrdude), at opts->baud; until cancelled, and
 * only from where the portal opens without a password. */
bool aos_flasher_bridge(const char *port, const aos_flasher_opts_t *opts);
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
    bool     secure;            /* secure boot or flash encryption on; SWD: read protection */
    bool     lines;             /* the port has EN/BOOT */
    aos_flasher_family_t family;
    char     boot[32];          /* an AVR: its bootloader, "optiboot 8.0, 115200"; SWD: the core, "Cortex-M4, DP v2" */
    uint16_t page;              /* an AVR: the flash page, bytes */
    /* a dump: where it went (kept after the job; removed if the job failed),
     * the address it started at and its SHA-256 in hex */
    char     out_path[176];
    uint32_t out_from;
    char     out_sha256[65];
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

/* ---- images (aos_flasher_img.c) ----
 * What a file asks to be written where, for the families that write
 * microcontrollers (SWD, AVR, STM32): a .bin at an address given by the
 * caller (or its name's "@0x"), Intel HEX (what Arduino and most IDEs give),
 * ELF (the segments that load, at their load address) and UF2 (the Pico's).
 * The file is read whole into PSRAM and turned into regions: sorted by
 * address, never overlapping, with the gaps of up to 256 bytes between two
 * pieces filled with 0xFF (alignment padding, a hex file's short holes) so a
 * firmware is a few regions, not thousands. */
#define AOS_FLASHER_REGIONS_MAX 64

typedef enum { AOS_IMG_BIN = 0, AOS_IMG_HEX, AOS_IMG_ELF, AOS_IMG_UF2 } aos_flasher_img_fmt_t;

typedef struct {
    uint32_t addr, len;
    uint8_t *data;
} aos_flasher_region_t;

typedef struct {
    aos_flasher_img_fmt_t fmt;
    int      n;
    aos_flasher_region_t r[AOS_FLASHER_REGIONS_MAX];
    uint32_t total;             /* bytes in all the regions */
    uint32_t uf2_family;        /* a .uf2's family ID (0xE48BFF56: RP2040), 0 none */
    uint32_t entry;             /* an .elf's entry point, a .hex's start address; 0 none */
    void    *mem;               /* what aos_flasher_image_free() lets go */
} aos_flasher_image_t;

/* By the extension: .hex/.ihex, .elf/.axf, .uf2, anything else a .bin. */
aos_flasher_img_fmt_t aos_flasher_image_fmt(const char *path);
/* Loads 'path'; a .bin goes at bin_addr. false with a reason in err. */
bool aos_flasher_image_load(const char *path, uint32_t bin_addr, aos_flasher_image_t *img, char *err, size_t err_len);
void aos_flasher_image_free(aos_flasher_image_t *img);

#ifdef __cplusplus
}
#endif
