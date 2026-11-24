/*
 * P4OS - Vision: checks esp-dl's models against the test vectors each one
 * carries, on the Mac, with esp-dl's C kernels and our model reader
 * (engine/ve_fbs_model.cpp). A model that passes here is read right.
 *
 *   tools/ve_check.sh model.espdl ...
 *
 * Each .espdl is "EDL2", the encryption mode, the FlatBuffer's size and a
 * word of padding, then the FlatBuffer: read whole into an aligned buffer
 * and handed to dl::Model as an FbsModel, as the engine does on the board.
 */
#include "dl_model_base.hpp"
#include "fbs_model.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = nullptr;
    if (n > 16 && posix_memalign((void **)&buf, 16, (size_t)n) == 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = nullptr;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        uint8_t *file = read_file(argv[i], &len);
        if (!file || memcmp(file, "EDL2", 4) != 0) {
            printf("%-48s not an EDL2 model\n", argv[i]);
            bad++;
            free(file);
            continue;
        }
        uint32_t mode, size;
        memcpy(&mode, file + 4, 4);
        memcpy(&size, file + 8, 4);
        if (mode != 0 || size + 16 > len) {
            printf("%-48s encrypted or cut short\n", argv[i]);
            bad++;
            free(file);
            continue;
        }
        auto *fbs = new fbs::FbsModel(file + 16, size, fbs::MODEL_LOCATION_IN_SDCARD, false, false, false, false);
        if (getenv("VE_VERBOSE")) fbs->print();
        int64_t t0 = esp_timer_get_time();
        auto *model = new dl::Model(fbs);
        int64_t t1 = esp_timer_get_time();
        esp_err_t ok = model->test();
        int64_t t2 = esp_timer_get_time();
        printf("%-48s %s  (build %lld ms, run %lld ms)\n", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i],
               ok == ESP_OK ? "PASS" : "FAIL", (long long)(t1 - t0) / 1000, (long long)(t2 - t1) / 1000);
        if (ok != ESP_OK) bad++;
        delete model;
        delete fbs;
        free(file);
    }
    return bad ? 1 : 0;
}
