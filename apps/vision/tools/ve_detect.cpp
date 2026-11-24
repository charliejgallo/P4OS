/*
 * P4OS - Vision: runs one of the engine's detectors on a picture, on the
 * Mac, and draws what it found (tools/ve_check.sh --detect).
 *
 *   ve_check.sh --detect face|person|coco|hand in.ppm out.ppm model.espdl [model2.espdl]
 *
 * The picture is a binary PPM (ffmpeg -i x.jpg x.ppm); it goes to the
 * engine as RGB565, as the camera's frames do.
 */
#include "../main/ve_api.h"

#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_all(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = nullptr;
    if (posix_memalign((void **)&b, 16, (size_t)n) || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = nullptr;
    }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: ve_detect face|person|coco|hand in.ppm out.ppm model [model2]\n");
        return 2;
    }
    const char *kinds[] = {"face", "person", "coco", "hand"};
    int kind = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(argv[1], kinds[i])) kind = i;
    FILE *f = fopen(argv[2], "rb");
    int w, h, mx;
    if (kind < 0 || !f || fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3) return 2;
    fgetc(f);
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (fread(rgb, 3, (size_t)w * h, f) != (size_t)w * h) return 2;
    fclose(f);
    uint16_t *px = (uint16_t *)malloc((size_t)w * h * 2);
    for (int i = 0; i < w * h; i++)
        px[i] = (uint16_t)(((rgb[3 * i] & 0xf8) << 8) | ((rgb[3 * i + 1] & 0xfc) << 3) | (rgb[3 * i + 2] >> 3));

    const uint8_t *blobs[2] = {};
    uint32_t lens[2] = {};
    int nb = argc - 4 > 2 ? 2 : argc - 4;
    for (int i = 0; i < nb; i++) blobs[i] = read_all(argv[4 + i], &lens[i]);
    const ve_api_t *api = ve_module_api();
    char err[128];
    ve_net_t *net = api->open((ve_kind_t)kind, blobs, lens, nb, err, sizeof err);
    if (!net) {
        fprintf(stderr, "open: %s\n", err);
        return 1;
    }
    ve_box_t boxes[32];
    ve_times_t t;
    int n = api->run(net, px, w, h, w, boxes, 32, &t);
    printf("%d found (pre %u us, model %u us, post %u us)\n", n, t.pre_us, t.model_us, t.post_us);
    for (int i = 0; i < n; i++) {
        ve_box_t &b = boxes[i];
        printf("  %3d,%3d - %3d,%3d  score %.2f  cls %d (%.2f)\n", b.x0, b.y0, b.x1, b.y1, b.score, b.cls, b.cls_score);
        for (int x = b.x0; x <= b.x1; x++)
            for (int y : {(int)b.y0, (int)b.y1}) memcpy(rgb + 3 * ((size_t)y * w + x), "\x00\xff\x00", 3);
        for (int y = b.y0; y <= b.y1; y++)
            for (int x : {(int)b.x0, (int)b.x1}) memcpy(rgb + 3 * ((size_t)y * w + x), "\x00\xff\x00", 3);
    }
    f = fopen(argv[3], "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
    api->close(net);
    return 0;
}
