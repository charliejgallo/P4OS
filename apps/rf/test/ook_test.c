/*
 * ook_test - runs rf_ook.c over a cu8 file and prints every transmission
 * and what it decoded to (apps/rf/test/README.md):
 *
 *   ook_test <rate> <in.cu8>
 */
#include "rf_ook.h"

#include <stdio.h>
#include <stdlib.h>

static double s_t;

static void got(const rf_pulses_t *p, void *ctx)
{
    (void)ctx;
    rf_decoded_t d;
    bool ok = rf_ook_decode(p, &d);
    printf("%6.3f s  %3d marks  %5.1f dB  %+6d Hz  ", s_t, p->n, (double)p->snr_db, (int)p->offset_hz);
    if (!ok) printf("noise\n");
    else printf("%-9s %s  [%d bits %s]\n", d.proto[0] ? d.proto : "?", d.text, d.nbits, d.hex);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    unsigned rate = (unsigned)atol(argv[1]);
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    static uint8_t buf[2 * 16384];
    rf_ook_t *o = rf_ook_new(rate);
    long done = 0;
    size_t got_n;
    while ((got_n = fread(buf, 2, 16384, f)) > 0) {
        s_t = (double)done / rate;
        rf_ook_feed(o, buf, (int)got_n, got, NULL);
        done += (long)got_n;
    }
    rf_ook_free(o);
    fclose(f);
    return 0;
}
