/*
 * microSD (HARDWARE.md test 8): 4-bit SDMMC on the slot 0 IOMUX pins, card
 * power through the P4's LDO channel 4, like the BSP, but with the clock as a
 * parameter so 20 and 40 MHz can be compared.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_console.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "diskio_impl.h"
#include "ff.h"
#include <fcntl.h>
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "bench.h"

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static int s_khz;

bool sd_mounted(void) { return s_card != NULL; }

esp_err_t sd_mount(int freq_khz)
{
    if (s_card) return ESP_OK;
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
        esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (e != ESP_OK) return e;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = freq_khz;
    host.pwr_ctrl_handle = s_pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 64 * 1024 };
    esp_err_t e = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &s_card);
    if (e != ESP_OK) s_card = NULL;
    else s_khz = freq_khz;
    return e;
}

static void sd_umount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_card = NULL;
}

static void speed(int mb, int chunk_kb)
{
    mkdir("/sdcard/bench", 0777);
    const char *path = "/sdcard/bench/speed.bin";
    size_t chunk = chunk_kb * 1024;
    uint8_t *buf = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) buf = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_SPIRAM);
    for (size_t i = 0; i < chunk; i++) buf[i] = i * 31;
    size_t total = (size_t)mb << 20;

    FILE *f = fopen(path, "wb");
    if (!f) { bench_report("sd.speed", "error=open"); heap_caps_free(buf); return; }
    setvbuf(f, NULL, _IONBF, 0);
    int64_t t = bench_us();
    for (size_t done = 0; done < total; done += chunk) fwrite(buf, 1, chunk, f);
    fsync(fileno(f));
    fclose(f);
    double w = total / (double)(bench_us() - t);

    f = fopen(path, "rb");          /* buffered, as the apps read (CONFIG_FATFS_VFS_FSTAT_BLKSIZE) */
    t = bench_us();
    size_t got = 0, n;
    while ((n = fread(buf, 1, chunk, f)) > 0) got += n;
    fclose(f);
    double r = got / (double)(bench_us() - t);
    unlink(path);

    /* many small files: what the launcher, icons and language packs do */
    t = bench_us();
    char name[48];
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "/sdcard/bench/s%03d.txt", i);
        FILE *s = fopen(name, "wb");
        if (s) { fwrite(buf, 1, 4096, s); fclose(s); }
    }
    double small_w = (bench_us() - t) / 100000.0;
    t = bench_us();
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "/sdcard/bench/s%03d.txt", i);
        FILE *s = fopen(name, "rb");
        if (s) { fread(buf, 1, 4096, s); fclose(s); }
    }
    double small_r = (bench_us() - t) / 100000.0;
    for (int i = 0; i < 100; i++) { snprintf(name, sizeof name, "/sdcard/bench/s%03d.txt", i); unlink(name); }

    bench_report("sd.speed", "khz=%d mb=%d chunk_kb=%d write_MBps=%.2f read_MBps=%.2f small4k_write_ms=%.2f small4k_read_ms=%.2f",
                 s_khz, mb, chunk_kb, w, r, small_w, small_r);
    heap_caps_free(buf);
}

static int cmd_sd(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "info");
    if (!strcmp(sub, "mount")) {
        int khz = arg_int(argc, argv, 2, SDMMC_FREQ_HIGHSPEED);
        sd_umount();
        esp_err_t e = sd_mount(khz);
        bench_report("sd.mount", "khz=%d ok=%d err=%s", khz, e == ESP_OK, esp_err_to_name(e));
        return 0;
    }
    if (!strcmp(sub, "umount")) { sd_umount(); bench_say("sd unmounted"); return 0; }
    if (!sd_mounted() && sd_mount(SDMMC_FREQ_HIGHSPEED) != ESP_OK) { bench_report("sd", "error=no_card"); return 0; }

    if (!strcmp(sub, "info")) {
        sdmmc_card_print_info(stdout, s_card);
        uint64_t total = 0, freeb = 0;
        esp_vfs_fat_info("/sdcard", &total, &freeb);
        bench_report("sd.info", "name=%s size_mb=%llu free_mb=%llu khz=%d real_khz=%d width=%d ddr=%d",
                     s_card->cid.name, (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20),
                     s_khz, s_card->real_freq_khz, s_card->log_bus_width == 2 ? 4 : 1, s_card->is_ddr);
        return 0;
    }
    if (!strcmp(sub, "speed")) {
        int mb = arg_int(argc, argv, 2, 16);
        speed(mb, 32);
        speed(mb, 256);
        return 0;
    }
    if (!strcmp(sub, "raw")) {
        /* the bus without FAT: multi-block reads straight from the card */
        int mb = arg_int(argc, argv, 2, 8), blk_kb = arg_int(argc, argv, 3, 64);
        size_t chunk = (size_t)blk_kb * 1024, secs = chunk / 512;
        uint8_t *buf = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buf) { bench_report("sd.raw", "error=nomem"); return 0; }
        size_t start = 8192, n = ((size_t)mb << 20) / chunk;       /* 4 MB in, past the FAT */
        int64_t t = bench_us();
        esp_err_t e = ESP_OK;
        size_t i;
        for (i = 0; i < n && e == ESP_OK; i++) e = sdmmc_read_sectors(s_card, buf, start + i * secs, secs);
        int64_t us = bench_us() - t;
        bench_report("sd.raw", "mb=%d blk_kb=%d read_MBps=%.2f ms_per_blk=%.2f err=%s", mb, blk_kb,
                     (i * chunk) / (double)us, us / 1000.0 / (i ? i : 1), esp_err_to_name(e));
        heap_caps_free(buf);
        return 0;
    }
    if (!strcmp(sub, "rd")) {
        /* where the file read is slow: read() or fread(), and what FAT this is */
        int mb = arg_int(argc, argv, 2, 2), kb = arg_int(argc, argv, 3, 32);
        size_t chunk = (size_t)kb * 1024, total = (size_t)mb << 20;
        uint8_t *buf = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buf) { bench_report("sd.rd", "error=nomem"); return 0; }
        FATFS *fs = NULL;
        DWORD nfree = 0;
        char drv[4] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
        f_getfree(drv, &nfree, &fs);
        mkdir("/sdcard/bench", 0777);
        const char *path = "/sdcard/bench/rd.bin";
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        for (size_t d = 0; fd >= 0 && d < total; d += chunk) write(fd, buf, chunk);
        if (fd >= 0) close(fd);
        /* 1: read() */
        fd = open(path, O_RDONLY);
        int64_t t = bench_us(), worst = 0;
        size_t got = 0;
        int calls = 0;
        for (;;) {
            int64_t c0 = bench_us();
            int n = fd >= 0 ? read(fd, buf, chunk) : -1;
            int64_t c = bench_us() - c0;
            if (c > worst) worst = c;
            if (n <= 0) break;
            got += (size_t)n;
            calls++;
        }
        if (fd >= 0) close(fd);
        double r1 = got / (double)(bench_us() - t);
        /* 2: fread, default buffering */
        FILE *f = fopen(path, "rb");
        t = bench_us();
        size_t got2 = 0, n2;
        while (f && (n2 = fread(buf, 1, chunk, f)) > 0) got2 += n2;
        if (f) fclose(f);
        double r2 = got2 / (double)(bench_us() - t);
        /* 3: fread, unbuffered (what sd speed does) */
        f = fopen(path, "rb");
        if (f) setvbuf(f, NULL, _IONBF, 0);
        t = bench_us();
        size_t got3 = 0;
        while (f && (n2 = fread(buf, 1, chunk, f)) > 0) got3 += n2;
        if (f) fclose(f);
        double r3 = got3 / (double)(bench_us() - t);
        unlink(path);
        bench_report("sd.rd", "mb=%d kb=%d fs_type=%d csize_sect=%d read_MBps=%.2f calls=%d worst_ms=%.1f fread_MBps=%.2f fread_nobuf_MBps=%.2f",
                     mb, kb, fs ? fs->fs_type : -1, fs ? fs->csize : -1, r1, calls, worst / 1000.0, r2, r3);
        heap_caps_free(buf);
        return 0;
    }
    if (!strcmp(sub, "ls")) {
        const char *dir = arg_str(argc, argv, 2, "/sdcard");
        DIR *d = opendir(dir);
        if (!d) { printf("cannot open %s\n", dir); return 0; }
        struct dirent *e;
        while ((e = readdir(d))) printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
        closedir(d);
        return 0;
    }
    printf("sd info|mount [khz]|umount|speed [MB]|raw [MB] [blk_kb]|ls [dir]\n");
    return 0;
}

void reg_sd(void)
{
    const esp_console_cmd_t cmd = { .command = "sd", .help = "microSD tests (info mount umount speed ls)", .func = cmd_sd };
    esp_console_cmd_register(&cmd);
}
