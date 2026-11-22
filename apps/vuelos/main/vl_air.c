/*
 * VUELOS - the airports, from the pack <card>/apps/vuelos_p4.pak, made by
 * tools/gen_airports.py from OurAirports' airports.csv (public domain).
 *
 * Format (little-endian):
 *   0   "VLA1"
 *   4   u32 count
 *   8   count x 48 bytes, sorted by latitude:
 *         i32 lat, i32 lon     degrees x 1e5
 *         u8  kind             0 large, 1 medium, 2 small, 3 heliport
 *         char icao[4]         NUL-padded
 *         char iata[3]         NUL-padded
 *         char name[32]        UTF-8, NUL-padded
 *
 * Read whole into PSRAM once, when the app opens: some 40 bytes an airport.
 * Without the pack there are simply no airports on the map.
 */
#include "vuelos.h"

#include "vl_mem.h"

#include <stdio.h>
#include <string.h>

#define REC 48

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

void vl_air_load(app_t *a)
{
    a->nap = 0;
    a->ap_missing = true;
    char path[200];
    const char *dir = aos_hal_path_apps();
    snprintf(path, sizeof path, "%s/vuelos_p4.pak", dir ? dir : ".");
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        aos_hal_log("vuelos", "no %s: no airports", path);
        return;
    }
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, fp) != 8 || memcmp(hdr, "VLA1", 4)) {
        fclose(fp);
        return;
    }
    int n = le32(hdr + 4);
    if (n <= 0 || n > 200000) {
        fclose(fp);
        return;
    }
    a->ap = (vl_airport_t *)vl_malloc(sizeof(vl_airport_t) * (size_t)n);
    uint8_t *buf = (uint8_t *)vl_malloc(REC * 256);
    if (!a->ap || !buf) {
        vl_free(buf);
        fclose(fp);
        vl_air_free(a);
        return;
    }
    int got = 0;
    while (got < n) {
        int want = n - got < 256 ? n - got : 256;
        int r = (int)fread(buf, REC, (size_t)want, fp);
        if (r <= 0) break;
        for (int i = 0; i < r; i++) {
            const uint8_t *p = buf + i * REC;
            vl_airport_t *ap = &a->ap[got + i];
            ap->lat = le32(p);
            ap->lon = le32(p + 4);
            ap->kind = p[8];
            memcpy(ap->icao, p + 9, 4);
            ap->icao[4] = 0;
            memcpy(ap->iata, p + 13, 3);
            ap->iata[3] = 0;
            memcpy(ap->name, p + 16, 31);
            ap->name[31] = 0;
        }
        got += r;
    }
    vl_free(buf);
    fclose(fp);
    a->nap = got;
    a->ap_missing = false;
    aos_hal_log("vuelos", "%d airports from the pack", got);
}

void vl_air_free(app_t *a)
{
    vl_free(a->ap);
    a->ap = NULL;
    a->nap = 0;
}
