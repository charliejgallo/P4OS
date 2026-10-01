#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "aos_hal.h"
#include "driver/jpeg_decode.h"
#include "esp_jpeg_dec.h"

static void save(const char *name, const uint16_t *px, int w, int h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        uint16_t v = px[i];
        rgb[3 * i] = (v >> 8) & 0xF8; rgb[3 * i + 1] = (v >> 3) & 0xFC; rgb[3 * i + 2] = (v << 3) & 0xF8;
    }
    png_image img = { .version = PNG_IMAGE_VERSION, .width = w, .height = h, .format = PNG_FORMAT_RGB };
    png_image_write_to_file(&img, name, 0, rgb, 0, NULL);
    free(rgb);
}

int main(int argc, char **argv)
{
    /* args: in offset size max_w max_h fill out.png */
    const char *in = argv[1];
    uint32_t off = atoi(argv[2]), size = atoi(argv[3]);
    int mw = atoi(argv[4]), mh = atoi(argv[5]), fill = atoi(argv[6]);
    int ow = 0, oh = 0, sw = 0, sh = 0;
    uint16_t *px = aos_hal_image_decode(in, off, size, mw, mh, fill, &ow, &oh, &sw, &sh);
    printf("RESULT %s %s -> %dx%d (src %dx%d) hw=%d sw=%d sw_out=%dx%d\n", in, px ? "ok" : "NULL", ow, oh, sw, sh,
           fake_hw_calls, fake_sw_calls, fake_sw_last_w, fake_sw_last_h);
    if (px) { save(argv[7], px, ow, oh); aos_hal_image_free(px); }
    return px ? 0 : 1;
}
