/*
 * P4OS (from AmoledOS) - Video: MJPEG-in-AVI files, by their index.
 *
 * An AVI is a RIFF file: a 'hdrl' list with the headers (frame rate, frame
 * count, size), a 'movi' list with one chunk per frame ('00dc'), and an
 * index of those chunks. The watch read the frames in order and never
 * looked at the index; P4OS has a seek bar, so the index is read once at
 * open and every frame is then one read at a known place:
 *
 *   'indx'   OpenDML's index of indexes (files over 1 GB, where the frames
 *            go on in 'AVIX' RIFFs after the first one): each entry points
 *            to an 'ix00' chunk listing the frames of one RIFF.
 *   'idx1'   the classic one, after 'movi' (what ffmpeg writes for any file
 *            under 1 GB). Its offsets are counted from the 'movi' fourcc,
 *            or from the start of the file in some writers; the first entry
 *            says which.
 *   none     the chunks are walked once, header by header. Slow on a long
 *            file (a read per frame), but it plays.
 *
 * The index keeps where each frame's chunk HEADER is, so reading header and
 * data in one go leaves the file exactly at the next frame's header: plain
 * sequential playback never seeks. apps/video/convert.sh writes exactly this
 * shape (video only; the sound goes to a .wav beside it, see video.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t off;       /* the chunk's 8-byte header, from the start of the file */
    uint32_t size;      /* the JPEG; 0 is a frame that repeats the previous one */
} vd_frame_t;

typedef struct {
    FILE       *file;
    uint32_t    rate, scale;    /* frames per second = rate / scale (strh) */
    uint32_t    frames;         /* in the index; header-only: avih / dmlh  */
    uint16_t    width, height;
    uint32_t    max_size;       /* the biggest frame, bytes */
    vd_frame_t *index;          /* NULL when opened for the header only */
    vd_frame_t  first;          /* the first frame, also with no index */
    long        file_pos;       /* where the OS file position really is */
    char        stream[2];      /* the video stream's two digits: "00" */
} vd_avi_t;

/* Opens and reads the headers and, with 'index', the whole index. false if
 * it is not an AVI with an MJPEG video stream. */
bool vd_avi_open(vd_avi_t *avi, const char *path, bool index);
void vd_avi_close(vd_avi_t *avi);

/* The frame's time, and the frame on screen at a time. */
uint32_t vd_avi_frame_ms(const vd_avi_t *avi, uint32_t frame);
uint32_t vd_avi_frame_at(const vd_avi_t *avi, uint64_t ms);
uint32_t vd_avi_duration_ms(const vd_avi_t *avi);

/* Room a read of any frame needs (see vd_avi_read). */
size_t vd_avi_buf_size(const vd_avi_t *avi);

/* Reads frame 'i' into 'buf' (vd_avi_buf_size bytes, aligned to 128) and
 * points *jpeg at it inside. Returns the JPEG's size, 0 for an empty frame,
 * -1 on a read error. */
int vd_avi_read(vd_avi_t *avi, uint32_t i, uint8_t *buf, const uint8_t **jpeg);
