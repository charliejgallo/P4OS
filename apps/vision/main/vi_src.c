/*
 * P4OS - Vision: where the frames come from.
 *
 *   The webcam on the USB host (aos_hal_uvc_*, as the Cameras app uses it):
 *   MJPEG at VI_W x VI_H or the largest size below, the newest frame each
 *   time.
 *
 *   Test pictures, for the simulator and for a board with no camera: the
 *   card's vision/ folder. Each .jpg is shown for a few seconds, handed over
 *   again and again so the detector keeps running (and its fps mean
 *   something); an .mjpeg (JPEGs one after another, as ffmpeg -f mjpeg
 *   writes them) plays at 15 frames a second. Hidden files (._ of the Mac)
 *   are skipped. The webcam wins when both are there, unless the test
 *   pictures are asked for.
 */
#include "vision.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FRAME_CAP   (512 * 1024)
#define MJPEG_CAP   (24 * 1024 * 1024)
#define MAX_FILES   48
#define PICTURE_MS  4000
#define MJPEG_MS    66
#define TEST_MIN_MS 33          /* the same picture at most 30 times a second */

void *vi_alloc(size_t n);
void vi_free(void *p);

static struct {
    vi_src_kind_t kind;
    char name[48];
    /* camera */
    uint8_t *buf;
    uint32_t seq;
    /* test pictures */
    char files[MAX_FILES][64];
    int nfiles, cur;
    uint8_t *data;          /* the current file */
    int len;
    int *starts, *ends;     /* an .mjpeg's frames */
    int nframes, frame;
    uint32_t since_ms, last_ms;
} S;

const char *vi_test_dir(void)
{
    static char dir[128];
    const char *root = aos_hal_path_sd_root();
    snprintf(dir, sizeof dir, "%.100s/vision", root ? root : "/sdcard");
    return dir;
}

static bool is_mjpeg(const char *n)
{
    size_t l = strlen(n);
    return l > 6 && strcasecmp(n + l - 6, ".mjpeg") == 0;
}

static bool is_jpeg(const char *n)
{
    size_t l = strlen(n);
    return (l > 4 && strcasecmp(n + l - 4, ".jpg") == 0) || (l > 5 && strcasecmp(n + l - 5, ".jpeg") == 0);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void list_tests(void)
{
    S.nfiles = 0;
    DIR *d = opendir(vi_test_dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && S.nfiles < MAX_FILES) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof S.files[0]) continue;
        if (!is_jpeg(e->d_name) && !is_mjpeg(e->d_name)) continue;
        snprintf(S.files[S.nfiles++], sizeof S.files[0], "%.63s", e->d_name);
    }
    closedir(d);
    qsort(S.files, (size_t)S.nfiles, sizeof S.files[0], cmp_names);
}

static void free_file(void)
{
    vi_free(S.data);
    free(S.starts);
    free(S.ends);
    S.data = NULL;
    S.starts = S.ends = NULL;
    S.len = S.nframes = S.frame = 0;
}

/* the next test file that reads; false if none does */
static bool load_next(void)
{
    for (int tries = 0; tries < S.nfiles; tries++) {
        free_file();
        S.cur = (S.cur + 1) % S.nfiles;
        char path[200];
        snprintf(path, sizeof path, "%.120s/%s", vi_test_dir(), S.files[S.cur]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        int cap = is_mjpeg(S.files[S.cur]) ? MJPEG_CAP : FRAME_CAP;
        if (n < 4 || n > cap || !(S.data = (uint8_t *)vi_alloc((size_t)n))) {
            fclose(f);
            continue;
        }
        bool ok = fread(S.data, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        if (!ok) continue;
        S.len = (int)n;
        if (is_mjpeg(S.files[S.cur])) {
            /* the frames: SOI to EOI; a marker's bytes are never 0xFF 0xD8
             * inside the entropy-coded data (0xFF there is stuffed) */
            int cap_frames = 4096;
            S.starts = (int *)malloc(sizeof(int) * cap_frames);
            S.ends = (int *)malloc(sizeof(int) * cap_frames);
            int start = -1;
            for (int i = 0; S.starts && S.ends && i + 1 < S.len && S.nframes < cap_frames; i++) {
                if (S.data[i] != 0xFF) continue;
                if (S.data[i + 1] == 0xD8 && start < 0) start = i;
                else if (S.data[i + 1] == 0xD9 && start >= 0) {
                    S.starts[S.nframes] = start;
                    S.ends[S.nframes++] = i + 2;
                    start = -1;
                }
            }
            if (!S.nframes) continue;
        }
        snprintf(S.name, sizeof S.name, "%.47s", S.files[S.cur]);
        S.since_ms = (uint32_t)aos_hal_uptime_ms();
        return true;
    }
    free_file();
    return false;
}

bool vi_src_open(bool prefer_test)
{
    memset(&S, 0, sizeof S);
    S.cur = -1;
    list_tests();
    if (!(prefer_test && S.nfiles) && aos_hal_uvc_count() > 0) {
        uint16_t sizes[12][2];
        int ns = 0;
        char nm[48] = "";
        aos_hal_uvc_info(0, nm, sizeof nm, sizes, 12, &ns);
        if (aos_hal_uvc_start(0, VI_W, VI_H)) {
            S.buf = (uint8_t *)aos_hal_io_alloc(FRAME_CAP);
            if (S.buf) {
                S.kind = VI_SRC_CAMERA;
                snprintf(S.name, sizeof S.name, "%s", nm[0] ? nm : "USB");
                return true;
            }
            aos_hal_uvc_stop();
        }
    }
    if (S.nfiles && load_next()) {
        S.kind = VI_SRC_TEST;
        return true;
    }
    S.kind = VI_SRC_NONE;
    return false;
}

int vi_src_next(const uint8_t **jpeg, int *len)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (S.kind == VI_SRC_CAMERA) {
        int n = aos_hal_uvc_frame(S.buf, FRAME_CAP, &S.seq);
        if (n <= 0) return n;
        *jpeg = S.buf;
        *len = n;
        return 1;
    }
    if (S.kind != VI_SRC_TEST) return -1;
    if (S.nframes) {
        if (now - S.last_ms < MJPEG_MS) return 0;
        if (++S.frame >= S.nframes) {
            if (S.nfiles > 1 && !load_next()) return -1;
            S.frame = 0;
        }
        S.last_ms = now;
        *jpeg = S.data + S.starts[S.frame];
        *len = S.ends[S.frame] - S.starts[S.frame];
        return 1;
    }
    if (now - S.last_ms < TEST_MIN_MS) return 0;
    if (now - S.since_ms > PICTURE_MS && S.nfiles > 1 && !load_next()) return -1;
    if (S.nframes) return vi_src_next(jpeg, len);
    S.last_ms = now;
    *jpeg = S.data;
    *len = S.len;
    return 1;
}

void vi_src_close(void)
{
    if (S.kind == VI_SRC_CAMERA) aos_hal_uvc_stop();
    if (S.buf) aos_hal_io_free(S.buf);
    S.buf = NULL;
    free_file();
    S.kind = VI_SRC_NONE;
}

vi_src_kind_t vi_src_kind(void)
{
    return S.kind;
}

const char *vi_src_name(void)
{
    return S.name;
}
