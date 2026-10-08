/*
 * mesh_test - runs rf_mesh.c over a cu8 recording and prints the Meshtastic
 * frame of each packet (apps/rf/test/README.md). Mirrors ver.py.
 *
 *   mesh_test <file.cu8> <key>
 *
 * Fixed to the known capture: 960 ksps, 926.125 MHz, the MediumFast packets
 * at -20.6 kHz (SF9/250k), at the times below.
 */
#include "rf_mesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "mesh_test <file.cu8> <key>\n"); return 2; }
    const uint32_t rate = 960000, freq = 926125000, bw = 250000;
    const int32_t fc = -20600;
    const int sf = 9;
    const double t0s[] = { 30.23, 31.276, 41.994, 43.016, 47.93, 51.408, 51.89, 55.01 };

    uint8_t key[32];
    int keylen = rf_mesh_key(argv[2], key);
    printf("clave: %d bytes\n", keylen);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    int n = (int)(0.45 * rate);
    uint8_t *iq = malloc((size_t)n * 2);

    for (size_t t = 0; t < sizeof t0s / sizeof t0s[0]; t++) {
        long off = (long)((t0s[t] - 0.06) * rate) * 2;
        if (fseek(f, off, SEEK_SET) != 0) break;
        size_t got = fread(iq, 2, n, f);
        rf_mesh_t out;
        bool ok = rf_mesh_decode(iq, (int)got, rate, fc, freq, bw, sf, keylen ? key : NULL, keylen, &out);
        if (!ok) { printf("%7.3fs  sin CRC\n", t0s[t]); continue; }
        printf("%7.3fs  !%08x -> !%08x  saltos %d  ", t0s[t], out.from, out.to, out.hop_limit);
        if (out.portnum == 1) printf("TEXTO: %s\n", out.text);
        else if (out.decrypted) printf("(port %d, %d bytes)\n", out.portnum, out.payload_len);
        else printf("(sin clave)\n");
    }
    free(iq);
    fclose(f);
    return 0;
}
