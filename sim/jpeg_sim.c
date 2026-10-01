/*
 * P4OS simulator - JPEG frames in memory for the Cameras app, over
 * libavcodec (aos_hal_jpeg_*; the board's is components/aos_hal/aos_jpeg_p4.c).
 *
 * The same contract as the board: one decoder per stream, its own output
 * buffer, RGB565 in LVGL's byte order (little-endian here, as on the P4) at
 * the picture's own size. libavcodec reads progressive JPEGs and odd chroma
 * sampling that the P4's engine refuses (the board then falls back to
 * esp_new_jpeg in software): what decodes here is a superset.
 *
 * save() encodes with libavcodec's MJPEG encoder, which writes a plain
 * baseline JFIF file, the same kind esp_new_jpeg writes on the board.
 */
#include "aos_hal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct aos_jpeg {
    AVCodecContext    *ctx;
    AVPacket          *pkt;
    AVFrame           *frame;
    struct SwsContext *sws;
    uint16_t          *out;
    size_t             out_cap;
};

aos_jpeg_t *aos_hal_jpeg_open(void)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return NULL;
    aos_jpeg_t *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->ctx = avcodec_alloc_context3(codec);
    j->ctx->thread_count = 1;
    if (avcodec_open2(j->ctx, codec, NULL) < 0) {
        avcodec_free_context(&j->ctx);
        free(j);
        return NULL;
    }
    j->pkt = av_packet_alloc();
    j->frame = av_frame_alloc();
    return j;
}

/* libswscale wants the "J" (full range) formats named as the plain ones. */
static enum AVPixelFormat unj(enum AVPixelFormat f, bool *full)
{
    *full = true;
    switch (f) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
    default: *full = false; return f;
    }
}

const uint16_t *aos_hal_jpeg_decode(aos_jpeg_t *j, const void *data, size_t len,
                                    int *w, int *h, int *stride_px)
{
    if (!j || !data || len < 4 || len > 16u * 1024 * 1024) return NULL;
    if (av_new_packet(j->pkt, (int)len) < 0) return NULL;
    memcpy(j->pkt->data, data, len);
    int r = avcodec_send_packet(j->ctx, j->pkt);
    av_packet_unref(j->pkt);
    if (r < 0 || avcodec_receive_frame(j->ctx, j->frame) < 0) return NULL;
    int fw = j->frame->width, fh = j->frame->height;
    if (fw <= 0 || fh <= 0 || fw > 4096 || fh > 4096) return NULL;
    size_t need = (size_t)fw * fh * 2;
    if (need > j->out_cap) {
        free(j->out);
        j->out = malloc(need);
        j->out_cap = j->out ? need : 0;
        if (!j->out) return NULL;
    }
    bool full;
    enum AVPixelFormat src = unj(j->frame->format, &full);
    j->sws = sws_getCachedContext(j->sws, fw, fh, src, fw, fh, AV_PIX_FMT_RGB565LE,
                                  SWS_POINT, NULL, NULL, NULL);
    if (!j->sws) return NULL;
    if (full) {
        const int *coef = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(j->sws, coef, 1, coef, 1, 0, 1 << 16, 1 << 16);
    }
    uint8_t *dst[4] = { (uint8_t *)j->out };
    int dst_stride[4] = { fw * 2 };
    sws_scale(j->sws, (const uint8_t *const *)j->frame->data, j->frame->linesize, 0, fh, dst, dst_stride);
    av_frame_unref(j->frame);
    *w = fw;
    *h = fh;
    *stride_px = fw;
    return j->out;
}

void aos_hal_jpeg_close(aos_jpeg_t *j)
{
    if (!j) return;
    sws_freeContext(j->sws);
    av_frame_free(&j->frame);
    av_packet_free(&j->pkt);
    avcodec_free_context(&j->ctx);
    free(j->out);
    free(j);
}

/* mkdir -p of the folder that holds 'path'. */
static void make_parent(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

bool aos_hal_jpeg_save(const char *path, const uint16_t *px, int w, int h, int stride_px, int quality)
{
    if (!path || !px || w <= 0 || h <= 0 || stride_px < w) return false;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;
    bool ok = false;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = NULL;
    if (!ctx || !fr || !pkt) goto out;
    ctx->width = w & ~1;
    ctx->height = h & ~1;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = (AVRational){ 1, 25 };
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    /* quality 1-100 -> qscale 31..2 */
    int q = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    ctx->global_quality = FF_QP2LAMBDA * (31 - (q * 29) / 100);
    if (avcodec_open2(ctx, codec, NULL) < 0) goto out;
    fr->format = ctx->pix_fmt;
    fr->width = ctx->width;
    fr->height = ctx->height;
    fr->quality = ctx->global_quality;
    if (av_frame_get_buffer(fr, 0) < 0) goto out;
    sws = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_RGB565LE, ctx->width, ctx->height,
                         AV_PIX_FMT_YUV420P, SWS_POINT, NULL, NULL, NULL);
    if (!sws) goto out;
    {
        const int *coef = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(sws, coef, 1, coef, 1, 0, 1 << 16, 1 << 16);
        const uint8_t *src[4] = { (const uint8_t *)px };
        int src_stride[4] = { stride_px * 2 };
        sws_scale(sws, src, src_stride, 0, ctx->height, fr->data, fr->linesize);
    }
    if (avcodec_send_frame(ctx, fr) < 0 || avcodec_send_frame(ctx, NULL) < 0) goto out;
    if (avcodec_receive_packet(ctx, pkt) < 0) goto out;
    make_parent(path);
    {
        FILE *f = fopen(path, "wb");
        if (!f) goto out;
        ok = fwrite(pkt->data, 1, (size_t)pkt->size, f) == (size_t)pkt->size;
        ok = (fclose(f) == 0) && ok;
    }
out:
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&ctx);
    return ok;
}
