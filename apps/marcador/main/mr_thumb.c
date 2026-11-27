/*
 * Marcador - the little picture of each kind, for the list and the form:
 * 112 x 112, drawn with the same shapes as the scenes but much simpler.
 */
#include "mr_app.h"

#include <math.h>
#include <string.h>

static void bg_noise(mr_img_t *im, uint32_t c, float amp, uint32_t salt)
{
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float n = mr_vnoise(x / 9.0f, y / 9.0f, salt) - 0.5f + (mr_hashf(x, y, salt) - 0.5f) * 0.5f;
            float v = 1.0f - 0.35f * ((x - 56.0f) * (x - 56.0f) + (y - 50.0f) * (y - 50.0f)) / (80.0f * 80.0f);
            im->px[y * im->w + x] = 0xFF000000u | mr_rgb_scale(c, (1.0f + n * amp) * v);
        }
    }
}

static void wood(mr_img_t *im, uint32_t c, uint32_t salt)
{
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float n = mr_vnoise(x / 30.0f, y / 6.0f, salt);
            float r = sinf(y * 0.55f + n * 7.0f) * 0.5f + 0.5f;
            float k = 0.82f + 0.22f * r + (mr_hashf(x, y, salt) - 0.5f) * 0.06f;
            im->px[y * im->w + x] = 0xFF000000u | mr_rgb_scale(c, k);
        }
    }
}

static void round_corners(mr_img_t *im, float rad)
{
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float d = mr_rrect_sd(x + 0.5f, y + 0.5f, 0, 0, (float)im->w, (float)im->h, rad);
            float c = mr_clampf(0.5f - d, 0, 1);
            uint32_t p = im->px[y * im->w + x];
            uint32_t a = (uint32_t)((float)(p >> 24) * c);
            im->px[y * im->w + x] = (a << 24) | (p & 0xFFFFFF);
        }
    }
}

static void match(mr_img_t *im, float ax, float ay, float bx, float by)
{
    mr_img_capsule(im, ax + 2, ay + 3, bx + 2, by + 3, 3.2f, 0x000000, 0.35f);
    mr_img_capsule(im, ax, ay, bx, by, 2.6f, 0xE6C68C, 1.0f);
    mr_img_disc(im, bx, by, 4.0f, 0xA8301F, 1.0f);
}

static void bean(mr_img_t *im, float x, float y, float rot, uint32_t c)
{
    float ca = cosf(rot), sa = sinf(rot);
    for (int py = (int)y - 12; py <= (int)y + 12; py++) {
        for (int px = (int)x - 12; px <= (int)x + 12; px++) {
            float dx = px + 0.5f - x, dy = py + 0.5f - y;
            float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            float d = (u * u) / (9.5f * 9.5f) + (v * v) / (6.2f * 6.2f);
            float sd = ((u - 2) * (u - 2)) / (9.5f * 9.5f) + ((v - 3) * (v - 3)) / (6.2f * 6.2f);
            if (sd < 1.0f) mr_img_over(im, px, py, 0, 0.35f * (1 - sd));
            if (d < 1.0f) {
                float l = 0.75f + 0.35f * (1.0f - sqrtf(((u + 3) * (u + 3)) / 90.0f + ((v + 2) * (v + 2)) / 40.0f));
                mr_img_over(im, px, py, mr_rgb_scale(c, l), mr_clampf((1 - d) * 6, 0, 1));
            }
        }
    }
}

void mr_thumb(mr_img_t *im, int type)
{
    memset(im->px, 0, (size_t)im->w * im->h * 4);
    switch (type) {
    case MR_T_PANO:
        bg_noise(im, 0x1C6A3A, 0.25f, 3);
        match(im, 30, 80, 30, 34);
        match(im, 34, 30, 80, 30);
        match(im, 84, 34, 84, 80);
        match(im, 80, 84, 34, 84);
        match(im, 36, 78, 78, 36);
        break;
    case MR_T_POROTOS:
        wood(im, 0x9A6A3C, 5);
        {
            static const float B[9][3] = { { 40, 46, 0.3f }, { 58, 40, 1.4f }, { 52, 58, 2.2f }, { 70, 54, 0.8f },
                                           { 36, 66, 1.9f }, { 62, 72, 0.1f }, { 80, 70, 2.8f }, { 46, 82, 1.1f },
                                           { 74, 88, 0.6f } };
            for (int i = 0; i < 9; i++) bean(im, B[i][0], B[i][1], B[i][2], i % 3 == 1 ? 0xE8DCC0 : 0xF2EAD6);
        }
        break;
    case MR_T_CUENTA:
        bg_noise(im, 0x3A3F46, 0.2f, 9);
        mr_img_disc(im, 58, 60, 38, 0x000000, 0.4f);
        mr_img_disc(im, 56, 57, 38, 0x9AA2AC, 1.0f);
        mr_img_disc(im, 56, 57, 34, 0xD6DCE2, 1.0f);
        mr_img_disc(im, 50, 50, 20, 0xF4F7FA, 0.6f);
        mr_img_rrect(im, 50, 8, 12, 16, 3, 0xC0C6CC, 1.0f);
        mr_img_rrect(im, 28, 48, 56, 22, 3, 0x15171A, 1.0f);
        mr_text_img(im, "0042", 31, 52, 14, 2.2f, 0xF4F4F4);
        mr_img_disc(im, 56, 98, 10, 0x9AA2AC, 1.0f);
        mr_img_disc(im, 56, 98, 6, 0x3A3F46, 1.0f);
        break;
    case MR_T_ESTADIO:
        bg_noise(im, 0x14161A, 0.2f, 11);
        for (int k = 0; k < 2; k++) {
            /* a 3 x 5 lamp digit each side: 2 and 1 */
            static const char *const D[2] = { "111001111100111", "010110010010111" };
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++) {
                    float x = (k ? 66 : 18) + c * 11.0f + 5, y = 34 + r * 11.0f;
                    bool on = D[k][r * 3 + c] == '1';
                    if (on) mr_img_disc(im, x, y, 7.5f, 0xFF9A1E, 0.28f);
                    mr_img_disc(im, x, y, 4.0f, on ? 0xFFD27A : 0x2A2620, 1.0f);
                }
        }
        mr_img_rrect(im, 34, 92, 44, 12, 3, 0x2A0A08, 1.0f);
        for (int i = 0; i < 4; i++) mr_img_rrect(im, 38 + i * 10, 95, 6, 6, 1, 0xFF3B2A, 1.0f);
        break;
    case MR_T_CARTEL:
        bg_noise(im, 0x1D4F86, 0.18f, 13);
        mr_img_rrect(im, 16, 30, 38, 58, 5, 0x000000, 0.35f);
        mr_img_rrect(im, 14, 26, 38, 58, 5, 0xF7F5EE, 1.0f);
        mr_img_rrect(im, 60, 30, 38, 58, 5, 0x000000, 0.35f);
        mr_img_rrect(im, 58, 26, 38, 58, 5, 0xF7F5EE, 1.0f);
        mr_glyph_img(im, '1', 22, 36, 22, 40, 6, 0x1A1A1A);
        mr_glyph_img(im, '5', 66, 36, 22, 40, 6, 0x1A1A1A);
        for (int i = 0; i < 2; i++) {
            mr_img_disc(im, 26 + i * 44, 22, 4, 0x888E96, 1.0f);
            mr_img_disc(im, 40 + i * 44, 22, 4, 0x888E96, 1.0f);
        }
        break;
    case MR_T_ABACO:
        wood(im, 0x7A4A26, 17);
        mr_img_rrect(im, 12, 18, 88, 78, 4, 0xE8D7B8, 1.0f);
        for (int r = 0; r < 4; r++) {
            float y = 32 + r * 17.0f;
            mr_img_capsule(im, 14, y, 98, y, 1.5f, 0x8A8F96, 1.0f);
            static const uint32_t C[4] = { 0xD6402F, 0xF0B429, 0x2F8FD6, 0x37A85A };
            int n = 3 + r;
            for (int b = 0; b < n; b++) {
                float x = b < 2 ? 22 + b * 12.0f : 90 - (n - 1 - b) * 12.0f;
                mr_img_disc(im, x + 1, y + 2, 6.5f, 0x000000, 0.3f);
                mr_img_disc(im, x, y, 6.0f, C[r], 1.0f);
                mr_img_disc(im, x - 2, y - 2, 2.0f, 0xFFFFFF, 0.5f);
            }
        }
        break;
    case MR_T_TIZA:
    default:
        bg_noise(im, 0x23302A, 0.3f, 19);
        for (int i = 0; i < 4; i++) {
            float x = 26 + i * 14.0f;
            mr_img_capsule(im, x, 32, x + 2, 82, 2.6f, 0xEDEDE6, 0.85f);
        }
        mr_img_capsule(im, 16, 72, 84, 40, 2.6f, 0xEDEDE6, 0.85f);
        for (int i = 0; i < 300; i++) {
            uint32_t h = mr_hash((uint32_t)i * 13u);
            mr_img_over(im, (int)(h % 112), (int)((h >> 8) % 112), 0x23302A, 0.5f);
        }
        break;
    }
    round_corners(im, 22);
}
