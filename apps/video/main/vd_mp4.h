/*
 * P4OS - Video: MP4 files with H.264 (vd_mp4.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct vd_mp4 vd_mp4_t;
/* NULL: not an MP4 with a video track it can read */
vd_mp4_t *vd_mp4_open(const char *path, int *w, int *h, uint32_t *duration_ms, uint32_t *frames);
/* sample i into buf (vd_mp4_buf_size bytes) as Annex B: its length, -1 on an error */
int       vd_mp4_sample(vd_mp4_t *r, uint32_t i, uint8_t *buf, int max, uint32_t *ms, bool *key);
uint32_t  vd_mp4_key_at(const vd_mp4_t *r, uint32_t i);     /* the last key frame at or before i */
size_t    vd_mp4_buf_size(const vd_mp4_t *r);
uint32_t  vd_mp4_frame_ms(const vd_mp4_t *r, uint32_t i);
void      vd_mp4_free(vd_mp4_t *r);
