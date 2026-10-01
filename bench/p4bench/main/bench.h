/*
 * p4bench - hardware bench for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
 *
 * Every test prints machine-readable lines:
 *
 *     BENCH <test> key=value key=value ...
 *
 * over the UART0 console (the "UART" USB-C port, CH343) and keeps them in a
 * ring that /api/bench serves once Wi-Fi is up. tools/bench_run.py on the Mac
 * drives the console and files the lines under bench/results/.
 *
 * The test numbers in comments refer to docs/plan/HARDWARE.md.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define LCD_W 720
#define LCD_H 1280
#define LCD_BPP 2
#define LCD_FB_BYTES (LCD_W * LCD_H * LCD_BPP)

/* ---- results (bench_log.c) ---- */
void bench_log_init(void);
/* One result line: "BENCH <test> <fmt...>". Also shown on screen. */
void bench_report(const char *test, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Free text for the person looking at the board ("touch the four corners"). */
void bench_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the ring (oldest first) into buf, returns bytes written. */
size_t bench_log_dump(char *buf, size_t cap);
/* Monotonic microseconds. */
int64_t bench_us(void);
/* Saved key/value settings (NVS namespace "p4bench"). */
int32_t bench_cfg_get(const char *key, int32_t def);
void bench_cfg_set(const char *key, int32_t val);
bool bench_cfg_get_str(const char *key, char *out, size_t cap);
void bench_cfg_set_str(const char *key, const char *val);

/* ---- display (b_display.c) ---- */
esp_err_t disp_init(void);
esp_lcd_panel_handle_t disp_panel(void);
int disp_num_fbs(void);
uint16_t *disp_fb(int i);
/* Makes frame buffer i the one scanned out, from the next frame on. */
void disp_show_fb(int i);
int disp_front(void);
/* Waits for the next "frame buffer done" event, returns false on timeout. */
bool disp_wait_frame(int timeout_ms);
void disp_set_trans_done(void (*cb)(void));   /* the DMA2D copy of draw_bitmap ended (ISR) */
uint32_t disp_frame_count(void);
/* Writes back [p, p+len) from the CPU cache so the DPI DMA sees it. */
void disp_cache_flush(const void *p, size_t len);
void disp_backlight(int percent);

/* ---- LVGL port (lvport.c) ---- */
typedef enum { COPY_CPU = 0, COPY_PPA = 1, COPY_DMA2D = 2 } lvport_copy_t;
void lvport_start(void);
bool lvport_lock(int timeout_ms);
void lvport_unlock(void);
/* Raw tests draw straight into the frame buffers: LVGL stops flushing. */
void lvport_pause(bool pause);
bool lvport_running(void);
int lvport_rotation(void);
void lvport_ui_log(const char *line);   /* adds a line to the on-screen log */
void lvport_ui_status(const char *text);
/* The GT911 handle (NULL if it did not answer) and a way to keep LVGL off it
 * while a touch test reads it directly. */
struct esp_lcd_touch_s;
struct esp_lcd_touch_s *lvport_touch(void);
void lvport_touch_hold(bool hold);

/* ---- command groups ---- */
void reg_sys(void);
void reg_display(void);
void reg_lvgl(void);
void reg_touch(void);
void reg_sd(void);
void reg_net(void);
void reg_audio(void);
void reg_codec(void);
void reg_io(void);
void reg_elf(void);

/* Helpers for commands */
int arg_int(int argc, char **argv, int i, int def);
const char *arg_str(int argc, char **argv, int i, const char *def);
bool sd_mounted(void);
esp_err_t sd_mount(int freq_khz);
