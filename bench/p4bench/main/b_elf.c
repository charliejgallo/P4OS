/*
 * ELF loader (HARDWARE.md test 19): the way P4OS loads its dynamic apps,
 * tried on RISC-V from PSRAM before any app is ported.
 *
 *   elf so <file.so> [n]   dlopen() from the SD (path relative to /sdcard),
 *                          call bench_so_main(n) and bench_so_float(n)
 *
 * bench/hello_so builds the test object. It calls back into the firmware
 * through bench_report() and bench_us(), which this file exports, the same
 * way AmoledOS apps reach the HAL through aos_symbols.c.
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_elf.h"
#include "esp_dlfcn.h"
#include "private/elf_symbol.h"
#include "bench.h"

static const struct esp_elfsym s_bench_syms[] = {
    ESP_ELFSYM_EXPORT(bench_report),
    ESP_ELFSYM_EXPORT(bench_say),
    ESP_ELFSYM_EXPORT(bench_us),
    ESP_ELFSYM_END
};

static int cmd_elf(int argc, char **argv)
{
    static bool registered;
    if (!registered) { esp_elf_register_symbol(s_bench_syms); registered = true; }
    if (argc < 3 || strcmp(argv[1], "so")) {
        printf("elf so <file.so relative to /sdcard> [n]\n");
        return 0;
    }
    if (!sd_mounted() && sd_mount(40000) != ESP_OK) { bench_report("elf", "error=no_sd"); return 0; }
    size_t psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t = bench_us();
    void *h = dlopen(argv[2], RTLD_NOW);
    int64_t open_us = bench_us() - t;
    if (!h) { bench_report("elf", "file=%s dlopen=0 error=\"%s\"", argv[2], dlerror()); return 0; }
    size_t psram_used = psram0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t int_used = int0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int (*fn)(int) = dlsym(h, "bench_so_main");
    float (*ffn)(int) = dlsym(h, "bench_so_float");
    int n = arg_int(argc, argv, 3, 1000000);
    int r = -1;
    float fr = -1;
    int64_t call_us = 0, fcall_us = 0;
    if (fn) { t = bench_us(); r = fn(n); call_us = bench_us() - t; }
    if (ffn) { t = bench_us(); fr = ffn(n); fcall_us = bench_us() - t; }
    bench_report("elf", "file=%s dlopen=1 open_ms=%.2f psram_used=%u int_used=%u main=%d result=%d call_ms=%.2f "
                 "float=%d fresult=%.4f fcall_ms=%.2f",
                 argv[2], open_us / 1000.0, (unsigned)psram_used, (unsigned)int_used, fn != NULL, r, call_us / 1000.0,
                 ffn != NULL, fr, fcall_us / 1000.0);
    dlclose(h);
    return 0;
}

void reg_elf(void)
{
    const esp_console_cmd_t cmd = { .command = "elf", .help = "elf so <file.so> [n]: dlopen from the SD and call it", .func = cmd_elf };
    esp_console_cmd_register(&cmd);
}
