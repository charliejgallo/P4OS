/*
 * Test object for `elf so` in p4bench (HARDWARE.md test 19).
 *
 * Integer work, float work (the P4 has a single-precision FPU, so these
 * should compile to fadd/fmul instead of the soft-float calls the Xtensa
 * .so files needed), a string in .rodata, a static in .data/.bss, and calls
 * back into the firmware.
 */
#include <stdint.h>
#include <stdio.h>

extern void bench_report(const char *test, const char *fmt, ...);
extern int64_t bench_us(void);

static int calls;               /* .bss */
static int seed = 12345;        /* .data */

__attribute__((visibility("default"))) int bench_so_main(int n)
{
    calls++;
    uint32_t x = seed;
    for (int i = 0; i < n; i++) x = x * 1664525u + 1013904223u;
    bench_report("elf.so", "hello_from=hello_so calls=%d n=%d lcg=%lu", calls, n, (unsigned long)x);
    return (int)(x & 0x7fffffff);
}

__attribute__((visibility("default"))) float bench_so_float(int n)
{
    float acc = 0.0f, v = 1.0f;
    for (int i = 1; i <= n; i++) { v = v * 0.999999f + 1.0f / (float)i; acc += v; }
    printf("hello_so: float loop done (%d)\n", n);
    return acc / (float)n;
}

/* Only so the throwaway firmware link of this project succeeds; hidden and
 * never called from the .so. */
void app_main(void) {}
