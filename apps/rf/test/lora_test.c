/*
 * lora_test - runs rf_lora.c over a cu8 file and prints every packet it
 * found, with the bandwidth and spreading factor its preamble showed
 * (apps/rf/test/README.md):
 *
 *   lora_test <rate> <in.cu8>
 *
 * The dechirp runs right after each feed here, where the app runs it on a
 * thread of its own.
 */
#include "rf_lora.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned s_rate;

static void got(const rf_lora_pkt_t *p, void *ctx)
{
    (void)ctx;
    const char *preset = rf_lora_preset(p->sf, p->bw_hz);
    printf("%6.3f s  %7.1f ms  %+7.1f kHz  width %5.1f kHz  %5.1f dB  ", (double)p->start / s_rate,
           p->dur_us / 1000.0, p->offset_hz / 1000.0, p->width_hz / 1000.0, (double)p->snr_db);
    if (p->sf)
        printf("SF%-2d BW %6.2f k  q %.2f  %s\n", p->sf, p->bw_hz / 1000.0, (double)p->quality, preset ? preset : "");
    else printf("not LoRa  (q %.2f)\n", (double)p->quality);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    s_rate = (unsigned)atol(argv[1]);
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    static uint8_t buf[2 * 16384];
    rf_lora_t *o = rf_lora_new(s_rate);
    if (!o) return 1;
    size_t n;
    while ((n = fread(buf, 2, 16384, f)) > 0) {
        rf_lora_feed(o, buf, (int)n, got, NULL);
        rf_lora_analyse_pending(o);
    }
    rf_lora_free(o);
    fclose(f);
    return 0;
}
