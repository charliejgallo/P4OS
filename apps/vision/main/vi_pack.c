/*
 * P4OS - Vision: the model packs and the engine module.
 *
 * A pack is vision_p4.pak (faces and people), vision_coco_p4.pak or
 * vision_manos_p4.pak in the card's apps folder (tools/mkpack.py writes
 * them; the Store brings the extras):
 *
 *   "VEPK" u16 version u16 count
 *   count x entry (40 bytes): char name[32]  u32 offset  u32 length
 *   data: each entry 16-byte aligned
 *
 * A mode's models are read whole into PSRAM, 16-byte aligned: the engine
 * runs on them in place, so they live until the mode changes.
 *
 * The engine is apps/vision/engine.so on the card, opened the first time a
 * detector is chosen and kept while the app lives. In the simulator it is
 * linked in (sim.cmake).
 */
#include "vision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
static void *vi_big_alloc(size_t n)
{
    return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void vi_big_free(void *p)
{
    heap_caps_free(p);
}
#else
static void *vi_big_alloc(size_t n)
{
    void *p = NULL;
    return posix_memalign(&p, 16, n) == 0 ? p : NULL;
}
static void vi_big_free(void *p)
{
    free(p);
}
#endif

/* ---- the engine ---------------------------------------------------------- */

#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)

const ve_api_t *vi_engine(char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    return ve_module_api();
}

void vi_engine_close(void) {}

#else

static aos_module_t *s_mod;
static const ve_api_t *s_api;

const ve_api_t *vi_engine(char *err, size_t errlen)
{
    if (s_api) return s_api;
    char path[200];
    snprintf(path, sizeof path, "%.150s/vision/engine.so", aos_hal_path_apps());
    s_mod = aos_hal_module_open(path, err, errlen);
    if (!s_mod) return NULL;
    const ve_api_t *(*get)(void) = (const ve_api_t *(*)(void))aos_hal_module_sym(s_mod, "ve_module_api");
    const ve_api_t *api = get ? get() : NULL;
    if (!api || api->abi != VE_ABI) {
        snprintf(err, errlen, "engine.so: %s", api ? "another version" : "no ve_module_api");
        aos_hal_module_close(s_mod);
        s_mod = NULL;
        return NULL;
    }
    s_api = api;
    return api;
}

void vi_engine_close(void)
{
    if (s_mod) aos_hal_module_close(s_mod);
    s_mod = NULL;
    s_api = NULL;
}

#endif

/* ---- the packs ------------------------------------------------------------- */

const char *vi_mode_pack(vi_mode_t m)
{
    switch (m) {
    case VI_M_FACE:
    case VI_M_PERSON:
        return "vision_p4.pak";
    case VI_M_COCO:
        return "vision_coco_p4.pak";
    case VI_M_HAND:
        return "vision_manos_p4.pak";
    default:
        return NULL;
    }
}

static void pack_path(vi_mode_t m, char *out, size_t n)
{
    snprintf(out, n, "%.150s/%s", aos_hal_path_apps(), vi_mode_pack(m));
}

bool vi_pack_present(vi_mode_t m)
{
    if (!vi_mode_pack(m)) return true;
    char path[200];
    pack_path(m, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

typedef struct {
    char name[32];
    uint32_t off, len;
} vi_entry_t;

static bool read_entry(FILE *f, int count, const char *name, vi_entry_t *out)
{
    for (int i = 0; i < count; i++) {
        uint8_t raw[40];
        if (fseek(f, 8 + 40 * i, SEEK_SET) != 0 || fread(raw, 1, 40, f) != 40) return false;
        if (strncmp((const char *)raw, name, 32) == 0) {
            memcpy(out->name, raw, 32);
            memcpy(&out->off, raw + 32, 4);
            memcpy(&out->len, raw + 36, 4);
            return true;
        }
    }
    return false;
}

int vi_models_load(vi_mode_t m, const uint8_t **blobs, uint32_t *lens, int max, char *err, size_t errlen)
{
    static const char *const names[][2] = {
        [VI_M_FACE] = {"face_msr", "face_mnp"},
        [VI_M_PERSON] = {"person", NULL},
        [VI_M_COCO] = {"coco", NULL},
        [VI_M_HAND] = {"hand", "gesture"},
    };
    if (m >= VI_M_CODES) return 0;
    char path[200];
    pack_path(m, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "no %s", vi_mode_pack(m));
        return -1;
    }
    uint8_t head[8];
    uint16_t count = 0;
    if (fread(head, 1, 8, f) != 8 || memcmp(head, "VEPK", 4) != 0) {
        snprintf(err, errlen, "%s: not a Vision pack", vi_mode_pack(m));
        fclose(f);
        return -1;
    }
    memcpy(&count, head + 6, 2);
    int n = 0;
    for (int k = 0; k < 2 && k < max && names[m][k]; k++) {
        vi_entry_t e;
        if (!read_entry(f, count, names[m][k], &e)) {
            /* the gesture model is optional */
            if (k > 0) break;
            snprintf(err, errlen, "%s: no %s", vi_mode_pack(m), names[m][k]);
            goto fail;
        }
        uint8_t *buf = (uint8_t *)vi_big_alloc(e.len);
        if (!buf) {
            snprintf(err, errlen, "no memory for %s (%u KB)", names[m][k], (unsigned)(e.len / 1024));
            goto fail;
        }
        blobs[n] = buf;
        lens[n] = e.len;
        n++;
        if (fseek(f, (long)e.off, SEEK_SET) != 0 || fread(buf, 1, e.len, f) != e.len) {
            snprintf(err, errlen, "%s: cut short", vi_mode_pack(m));
            goto fail;
        }
    }
    fclose(f);
    return n;
fail:
    fclose(f);
    vi_models_free(blobs, n);
    return -1;
}

void vi_models_free(const uint8_t **blobs, int n)
{
    for (int i = 0; i < n; i++) {
        vi_big_free((void *)blobs[i]);
        blobs[i] = NULL;
    }
}

/* for the frames too */
void *vi_alloc(size_t n);
void vi_free(void *p);
void *vi_alloc(size_t n)
{
    return vi_big_alloc(n);
}
void vi_free(void *p)
{
    vi_big_free(p);
}
