/*
 * P4OS - Vision, the engine: esp-dl's detectors behind the plain C table of
 * main/ve_api.h.
 *
 * Each kind is one or two dl::Model over the app's buffers (the .espdl files
 * of a pack), with esp-dl's own pre- and post-processing set up as its model
 * components do it (esp-dl/models/<model>/<model>.cpp, MIT): the same means
 * and scales, letterbox, anchors and thresholds. The models' feature maps go
 * to PSRAM (no internal RAM is asked for: internal_size 0).
 *
 * The image comes as the app decoded it, RGB565 in LVGL's byte order; esp-dl
 * wants it without padding at the end of the rows, so a padded frame is
 * packed into a buffer of the engine's first.
 */
#include "ve_api.h"

#include "dl_cls_postprocessor.hpp"
#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_detect_mnp_postprocessor.hpp"
#include "dl_detect_msr_postprocessor.hpp"
#include "dl_detect_pico_postprocessor.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "fbs_model.hpp"
#include "hand_gesture_cls_postprocessor.hpp"

#include <limits>
#include <stdio.h>
#include <string.h>

namespace {

/* One model and its two ends. */
struct Stage {
    fbs::FbsModel *fbs = nullptr;
    dl::Model *model = nullptr;
    dl::image::ImagePreprocessor *pre = nullptr;
    dl::detect::DetectPostprocessor *post = nullptr;
    dl::cls::ClsPostprocessor *cls = nullptr;

    void free_all()
    {
        delete post;
        delete cls;
        delete pre;
        delete model;
        delete fbs;
        *this = Stage();
    }
};

uint32_t now_us()
{
    return (uint32_t)esp_timer_get_time();
}

#if VE_UNPACK_FILTERS
} // namespace
namespace fbs {
int unpack_simd_filters(uint8_t *flatbuffer);
}
namespace {
#endif

/* "EDL2", mode, size, padding, then the FlatBuffer. */
bool open_stage(Stage &s, const uint8_t *blob, uint32_t len, char *err, size_t errlen)
{
    uint32_t mode, size;
    if (!blob || len < 16 || memcmp(blob, "EDL2", 4) != 0) {
        snprintf(err, errlen, "not an EDL2 model");
        return false;
    }
    memcpy(&mode, blob + 4, 4);
    memcpy(&size, blob + 8, 4);
    if (mode != 0 || size + 16 > len || ((uintptr_t)blob & 15)) {
        snprintf(err, errlen, "encrypted, cut short or unaligned model");
        return false;
    }
#if VE_UNPACK_FILTERS
    /* the simulator: a copy of the model, its filters unpacked for the C
     * kernels; the FbsModel frees it (auto_free) */
    uint8_t *copy = (uint8_t *)heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
    if (!copy) {
        snprintf(err, errlen, "no memory for the model");
        return false;
    }
    memcpy(copy, blob + 16, size);
    fbs::unpack_simd_filters(copy);
    s.fbs = new fbs::FbsModel(copy, size, fbs::MODEL_LOCATION_IN_SDCARD, false, false, true, false);
#else
    s.fbs = new fbs::FbsModel(blob + 16, size, fbs::MODEL_LOCATION_IN_SDCARD, false, false, false, false);
#endif
    s.model = new dl::Model(s.fbs, 0, dl::MEMORY_MANAGER_GREEDY);
    if (s.model->get_inputs().empty() || s.model->get_outputs().empty()) {
        snprintf(err, errlen, "the model did not build (out of memory, or an operator esp-dl lacks)");
        return false;
    }
    s.model->minimize();
    return true;
}

const std::vector<dl::detect::anchor_point_stage_t> POINT_STAGES = {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}};

} // namespace

struct ve_net {
    ve_kind_t kind;
    Stage s[2];
    float thr;
    uint16_t *packed = nullptr;   /* the frame without row padding */
    size_t packed_len = 0;
};

static void net_close(ve_net_t *net)
{
    if (!net) return;
    net->s[0].free_all();
    net->s[1].free_all();
    heap_caps_free(net->packed);
    delete net;
}

static ve_net_t *net_open(ve_kind_t kind, const uint8_t *const *blobs, const uint32_t *lens, int nblobs, char *err,
                          size_t errlen)
{
    char dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    int need = kind == VE_K_FACE ? 2 : 1;
    if (kind < 0 || kind >= VE_K_COUNT || nblobs < need) {
        snprintf(err, errlen, "kind %d wants %d models, got %d", (int)kind, need, nblobs);
        return nullptr;
    }
    ve_net_t *net = new ve_net_t();
    net->kind = kind;
    bool ok = open_stage(net->s[0], blobs[0], lens[0], err, errlen);
    if (ok && (kind == VE_K_FACE || (kind == VE_K_HAND && nblobs > 1)))
        ok = open_stage(net->s[1], blobs[1], lens[1], err, errlen);
    if (!ok) {
        net_close(net);
        return nullptr;
    }
    Stage &a = net->s[0], &b = net->s[1];
    switch (kind) {
    case VE_K_FACE:
        /* human_face_detect: MSR finds candidates at 120x160, MNP checks each at 48x48 */
        net->thr = 0.5f;
        a.pre = new dl::image::ImagePreprocessor(a.model, {0, 0, 0}, {1, 1, 1}, true);
        a.post = new dl::detect::MSRPostprocessor(
            a.model, a.pre, 0.5f, 0.5f, 10, {{8, 8, 9, 9, {{16, 16}, {32, 32}}}, {16, 16, 9, 9, {{64, 64}, {128, 128}}}});
        b.pre = new dl::image::ImagePreprocessor(b.model, {0, 0, 0}, {1, 1, 1}, true);
        b.post = new dl::detect::MNPPostprocessor(b.model, b.pre, 0.5f, 0.5f, 10, {{1, 1, 0, 0, {{48, 48}}}});
        break;
    case VE_K_PERSON:
        net->thr = 0.7f;
        a.pre = new dl::image::ImagePreprocessor(a.model, {0, 0, 0}, {1, 1, 1});
        a.post = new dl::detect::PicoPostprocessor(a.model, a.pre, 0.7f, 0.5f, 10, POINT_STAGES);
        break;
    case VE_K_COCO:
        net->thr = 0.25f;
        a.pre = new dl::image::ImagePreprocessor(a.model, {0, 0, 0}, {255, 255, 255});
        a.pre->enable_letterbox({114, 114, 114});
        a.post = new dl::detect::yolo11PostProcessor(a.model, a.pre, 0.25f, 0.7f, 10, POINT_STAGES);
        break;
    case VE_K_HAND:
        net->thr = 0.25f;
        a.pre = new dl::image::ImagePreprocessor(a.model, {0, 0, 0}, {255, 255, 255});
        a.pre->enable_letterbox({114, 114, 114});
        a.post = new dl::detect::ESPDetPostProcessor(a.model, a.pre, 0.25f, 0.5f, 10, POINT_STAGES);
        if (b.model) {
            b.pre = new dl::image::ImagePreprocessor(b.model, {123.675f, 116.28f, 103.53f}, {58.395f, 57.12f, 57.375f});
            b.cls = new dl::cls::HandGestureClsPostprocessor(b.model, 1, std::numeric_limits<float>::lowest(), true);
        }
        break;
    default:
        break;
    }
    return net;
}

static int gesture_index(const char *name)
{
    static const char *const names[VE_G_COUNT] = {"one", "two", "three", "four", "five", "like",
                                                  "ok", "no_gesture", "call", "dislike", "no_hand"};
    for (int i = 0; i < VE_G_COUNT; i++)
        if (name && strcmp(name, names[i]) == 0) return i;
    return VE_G_NONE;
}

static int net_run(ve_net_t *net, const uint16_t *px, int w, int h, int stride_px, ve_box_t *out, int max,
                   ve_times_t *times)
{
    if (!net || !px || w <= 0 || h <= 0 || stride_px < w) return -1;
    ve_times_t t = {};
    if (stride_px != w) {
        size_t need = (size_t)w * h * 2;
        if (need > net->packed_len) {
            heap_caps_free(net->packed);
            net->packed = (uint16_t *)heap_caps_aligned_alloc(16, need, MALLOC_CAP_SPIRAM);
            net->packed_len = net->packed ? need : 0;
            if (!net->packed) return -1;
        }
        for (int y = 0; y < h; y++) memcpy(net->packed + (size_t)y * w, px + (size_t)y * stride_px, (size_t)w * 2);
        px = net->packed;
    }
    dl::image::img_t img = {(void *)px, (uint16_t)w, (uint16_t)h, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE};

    Stage &a = net->s[0], &b = net->s[1];
    uint32_t t0 = now_us();
    a.pre->preprocess(img);
    uint32_t t1 = now_us();
    a.model->run();
    uint32_t t2 = now_us();
    a.post->clear_result();
    a.post->postprocess();
    a.post->nms();
    std::list<dl::detect::result_t> *res = &a.post->get_result(w, h);
    uint32_t t3 = now_us();
    t.pre_us += t1 - t0;
    t.model_us += t2 - t1;
    t.post_us += t3 - t2;

    if (net->kind == VE_K_FACE) {
        /* each candidate squared and checked by MNP, as human_face_detect's MNP::run */
        b.post->clear_result();
        for (auto &c : *res) {
            int cx = (c.box[0] + c.box[2]) >> 1, cy = (c.box[1] + c.box[3]) >> 1;
            int side = DL_MAX(c.box[2] - c.box[0], c.box[3] - c.box[1]);
            c.box[0] = cx - (side >> 1);
            c.box[1] = cy - (side >> 1);
            c.box[2] = c.box[0] + side;
            c.box[3] = c.box[1] + side;
            c.limit_box(w, h);
            uint32_t u0 = now_us();
            b.pre->preprocess(img, c.box);
            uint32_t u1 = now_us();
            b.model->run();
            uint32_t u2 = now_us();
            b.post->postprocess();
            uint32_t u3 = now_us();
            t.pre_us += u1 - u0;
            t.model_us += u2 - u1;
            t.post_us += u3 - u2;
        }
        b.post->nms();
        res = &b.post->get_result(w, h);
    }

    int n = 0;
    for (auto &r : *res) {
        if (n >= max) break;
        ve_box_t &o = out[n];
        memset(&o, 0, sizeof o);
        o.x0 = (int16_t)r.box[0];
        o.y0 = (int16_t)r.box[1];
        o.x1 = (int16_t)r.box[2];
        o.y1 = (int16_t)r.box[3];
        o.score = r.score;
        o.cls = (int16_t)r.category;
        int nk = (int)r.keypoint.size() / 2;
        if (nk > 5) nk = 5;
        o.nkp = (int16_t)nk;
        for (int k = 0; k < nk * 2; k++) o.kp[k] = (int16_t)r.keypoint[k];
        if (net->kind == VE_K_HAND && b.model) {
            std::vector<int> crop = {r.box[0], r.box[1], r.box[2], r.box[3]};
            uint32_t u0 = now_us();
            b.pre->preprocess(img, crop);
            uint32_t u1 = now_us();
            b.model->run();
            uint32_t u2 = now_us();
            std::vector<dl::cls::result_t> &g = b.cls->postprocess();
            uint32_t u3 = now_us();
            t.pre_us += u1 - u0;
            t.model_us += u2 - u1;
            t.post_us += u3 - u2;
            o.cls = (int16_t)(g.empty() ? VE_G_NONE : gesture_index(g[0].cat_name));
            o.cls_score = g.empty() ? 0 : g[0].score;
        }
        n++;
    }
    if (times) *times = t;
    return n;
}

static void net_set_threshold(ve_net_t *net, float score)
{
    if (!net || score <= 0 || score >= 1) return;
    net->thr = score;
    /* faces: the threshold is MNP's, the one that decides; MSR keeps its own */
    Stage &s = net->kind == VE_K_FACE ? net->s[1] : net->s[0];
    if (s.post) s.post->set_score_thr(score);
}

static float net_threshold(ve_net_t *net)
{
    return net ? net->thr : 0;
}

static const ve_api_t API = {
    VE_ABI, net_open, net_close, net_run, net_set_threshold, net_threshold,
};

#ifndef AOS_SIM
/* The loader does not run a module's .init_array (CAS found it first,
 * apps/cas/engine/cas_end.c): esp-dl's globals with constructors (this
 * file's POINT_STAGES, its operator registry) would stay empty. They run
 * once, before the first use, between two markers: this one, sorted first,
 * and ve_ctors_end.c's, the last object of the link. */
typedef void (*ve_ctor_t)(void);
__attribute__((used, section(".init_array.00000"))) static const ve_ctor_t ve_ctor_begin[1] = {0};
extern "C" const ve_ctor_t ve_ctor_end[];

static void run_ctors(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (const ve_ctor_t *f = ve_ctor_begin + 1; f < ve_ctor_end; f++)
        if (*f && *f != (ve_ctor_t)-1) (*f)();
}
#endif

extern "C" __attribute__((visibility("default"))) const ve_api_t *ve_module_api(void)
{
#ifndef AOS_SIM
    run_ctors();
#endif
    return &API;
}
