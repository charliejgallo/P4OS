/*
 * P4OS - audio on the Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
 *
 * The port of AmoledOS's audio block (aos_hal_esp32.c: tones, the player
 * with its gapless folder queue, the internet radio, remembering the last
 * track, the recorder, the raw microphone, the streaming speaker and the
 * mixer) to a board that has TWO codecs where the watch had one. The file
 * formats, the radio's reader and AAC are the same portable files as the
 * watch and the simulator: aos_audio.c, aos_radio.c, aos_aac_esp.c.
 *
 * ---- The hardware ---------------------------------------------------------
 *
 *   ES8311  mono DAC -> power amplifier -> speaker. I2C 0x18 (the driver's
 *           ES8311_CODEC_DEFAULT_ADDR 0x30 is the 8-bit form). The PA enable
 *           is GPIO53, and it is not ours to drive: the BSP hands it to the
 *           ES8311 driver (pa_pin), which raises it when the codec is opened
 *           and drops it when it is closed. So "the amp is on" == "the
 *           speaker device is open", and that is what the warm-up and the
 *           idle close below are about.
 *   ES7210  four-channel ADC, two microphones wired (MIC1, MIC2: the BSP's
 *           mic_selected = 0 means both). I2C 0x40 (0x80 in the 8-bit form).
 *           Not TDM with two mics: MIC1 is the left slot, MIC2 the right.
 *   One I2S port (I2S1: MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 11), created by
 *           bsp_audio_init() as a TX + RX pair. Full duplex: IDF makes the
 *           RX channel a slave of the TX one (BCLK/WS looped back inside
 *           the chip), so the microphones only have a clock while TX runs.
 *   I2C     the BSP's shared bus (bsp_i2c_init), the GT911's too. The
 *           i2c_master driver serialises transactions per bus, and codec
 *           traffic only happens on open, close, volume and gain, never per
 *           block: the touch's 30-60 reads a second are not delayed by more
 *           than one register write.
 *
 * On the watch the speaker and the microphone were the same ES8311 on one
 * IN_OUT codec device, and half of the code was arbitration: while the mic
 * captured, the speaker was left alone. That exclusion is gone. What the
 * hardware imposes instead is subtler: both directions share one clock.
 *
 * ---- The bus: 48 kHz, 16-bit, two slots, both ways, always -----------------
 *
 * Both codec devices are opened with the same format and it never changes.
 * The alternative was a rate that follows the content (44.1 kHz for most
 * MP3s, 16 kHz for the walkie-talkie) with the other direction converting.
 * It fails on the details of this driver stack:
 *   - esp_codec_dev refuses a second direction at another rate
 *     (audio_codec_data_i2s.c, check_fs_compatible: "conflict sample_rate"),
 *     and esp_codec_dev_open() ignores that result: the open "succeeds" and
 *     the second device runs at the first one's rate, silently wrong.
 *   - Changing the rate means disabling TX, and TX is the capture's clock:
 *     a gapless folder with a 48 kHz track after a 44.1 kHz one would tear a
 *     recording in progress, and so would the walkie switching directions.
 * With a fixed bus every transition is click-free and the codecs are
 * configured once. What it costs, by design:
 *   - Everything that is not 48 kHz goes through a polyphase resampler
 *     (Blackman-windowed sinc, 256 phases, Q14 integer arithmetic): music
 *     at 44.1/32/22.05 kHz in the decoder task (32 taps: 1.5 M multiply-
 *     adds per second of 48 kHz output), the app's stream (16 -> 48 kHz,
 *     32 taps) in the output task, and the microphone (48 kHz -> the rate
 *     asked for, 32 x the ratio in taps, up to 128: 96 for 16 kHz) in the
 *     capture task. Measured on the Mac with this same code: unity gain,
 *     flat to 0.8 of the lower Nyquist (15 kHz is -0.00 dB and 18 kHz
 *     -0.65 dB for 44.1 kHz music; 5 kHz -0.01 dB for a 16 kHz microphone),
 *     -3.5 dB at 0.875 of it (7 kHz for the 16 kHz microphone, 14 kHz for
 *     the meter's 32 kHz), images and aliases 56 to 91 dB down. For a
 *     speaker this size that is transparent; for the sound meter it is the
 *     top of its top octave to know about.
 *   - A 48 kHz track, a 48 kHz station and a 48 kHz capture pay nothing: the
 *     resampler is a copy when the rates match.
 *   - Twice the I2S traffic of a mono stream (the ES8311 plays one slot; the
 *     mono mix is written to both), 192 KB/s each way. Nothing, on this chip.
 *
 * One ordering rule comes out of the shared clock: the speaker is opened
 * BEFORE a capture joins the bus and stays open while it runs. Opening it
 * under a running capture goes through set_fmt(), which disables TX (and
 * with it the microphones' clock) for the reconfiguration: a gap in the
 * recording. Closing it is harmless (esp_codec_dev keeps TX running while
 * RX is enabled), reopening is not, so it is simply not done.
 *
 * ---- Tasks -----------------------------------------------------------------
 *
 *   aos_aout    core 0, prio 6, 6 KB internal, permanent. Brings the codecs
 *               up, then owns the ES8311: every 10 ms it mixes the music
 *               ring, the app's stream (resampled) and the tones into one
 *               stereo block and writes it. Opens the speaker on demand
 *               (60 ms of silence first: the amp's warm-up), closes it
 *               after 5 s with nothing to play and no capture running.
 *   aos_player  core 0 (1 while an app worker runs on 0), prio 2 / 5,
 *               6 KB internal. The watch's decoder, unchanged in spirit:
 *               file or station -> aos_audio -> mono -> 48 kHz -> ring,
 *               two seconds ahead; the writer (now the mixer) moves its
 *               priority. Reads the card and NVS: internal stack.
 *   aos_ain     core 0, prio 7, 4 KB internal, while a capture runs. Owns
 *               the ES7210: 10 ms blocks of stereo 48 kHz -> one microphone
 *               -> the capture's rate -> the capture ring. Nothing slow in
 *               it: the I2S DMA only holds 30 ms at 48 kHz (6 x 240 frames,
 *               the BSP's channel config), a third of what it held at the
 *               watch's 16 kHz.
 *   aos_mic     core 0, prio 6, 4 KB internal, while a capture runs. The
 *               watch's mic task (envelope, the app's PCM ring, the WAV),
 *               reading the capture ring instead of the codec: a slow card
 *               write stalls it, not the I2S, and two seconds of ring are
 *               there to absorb it.
 *   aos_radio   (aos_radio.c) unpinned, prio 3, 9 KB: the station's reader.
 * LVGL is on core 1 at priority 4 (aos_hal_p4.c); everything here keeps off
 * it. Every task that decodes or synthesises in float is pinned, so the
 * FPU anchoring (a task is pinned to the core of its first float) never
 * decides where it runs.
 *
 * ---- Buffers ---------------------------------------------------------------
 *
 * PSRAM: the music ring (2 s at 48 kHz mono, 192 KB), the decoder's chunk
 * and its resampled copy (~32 KB), the app's stream ring (1 s at its rate),
 * the capture ring (2 s at up to 48 kHz, 192 KB, never freed), the app's
 * microphone ring (1 s), the resamplers' tables (8-64 KB), the mixer's
 * blocks. Internal: the stacks above and the I2S DMA descriptors the BSP
 * allocates (2 x 6 x 960 B).
 *
 * ---- What changed in behaviour, against the watch --------------------------
 *
 *   - A beep sounds over the music, over the app's stream and over a
 *     capture (the tuner's reference tone: AOS_CAP_DUPLEX). On the watch it
 *     was dropped whenever something else owned the codec, the alarm
 *     included.
 *   - The streaming speaker opens while the microphone captures: no ~200 ms
 *     switch, and aos_hal_spk_open() no longer fails for it.
 *   - The music still pauses while an app listens (the recorder, the tuner,
 *     the walkie) or takes the speaker, and comes back on its own: that was
 *     policy, not hardware (music in the microphone ruins a tuner, a sound
 *     meter or a recording). With "Mix music and apps" on, the app's stream
 *     rides over the music at half volume, as on the watch.
 *   - Gapless across sample rates (the watch drained the ring and reopened
 *     the codec in between).
 *   - The microphone gain is the ES7210's PGA: 3 dB steps, 0..36, without
 *     33 (esp_codec_dev 1.5's get_db() maps 33 to its 30 dB code, so that
 *     step cannot be reached through the driver). The ES8311's was 6 dB
 *     steps to 42 (aos_hal.h describes the watch).
 *   - Mono capture from one microphone, MIC1, unless it looks dead (24 dB
 *     under MIC2 after half a second), then MIC2. Not the average: two
 *     microphones some centimetres apart summed are a comb filter for
 *     anything off-axis, which a tuner or a meter must not have. The HAL has
 *     no stereo request (aos_hal_rec_start takes a rate only), so there is
 *     no stereo recording to offer.
 *
 * ---- To measure on day one (the board had not arrived when this was written)
 *
 *   1. The amp's warm-up (OUT_WARMUP_MS, 60 ms, the watch's figure): the
 *      first 40 ms beep after 5 s of silence (Settings' test beep) must be
 *      whole. Lower it if the first note is fine at 20 ms; raise it if the
 *      attack is lost. Also whether the idle close (OUT_IDLE_MS) clicks.
 *   2. The real volume curve: ours (audio_hw_init: -36/-20/-10/-4/0 dB at
 *      1/25/50/75/100) through the ES8311 with hw_gain pa 5.0 V / dac 3.3 V
 *      (the BSP's). A 0 dBFS sine at 100 is clean on the board
 *      (2026-09-29); SPL at each point still to measure.
 *   3. The ES7210's PGA for the tuner and the sound meter (MIC_GAIN_DB, 30,
 *      the driver's own default): a voice at 50 cm, a clap at 1 m, silence.
 *      The end-of-recording log gives the peak, the clipped blocks and the
 *      mean level; the capture's log gives each microphone's level, which
 *      also says which slot is which microphone and whether one of them is
 *      an AEC reference instead of a microphone.
 *   4. CPU of MP3 and AAC/HE-AAC decode (decode_permille / load_permille in
 *      /api/player), with and without the 44.1 -> 48 kHz resampling.
 *   5. Latency of the streaming speaker (walkie: press to hear): ring +
 *      10 ms block + 30 ms of I2S DMA, plus the warm-up on a cold open.
 *   6. The capture: 'lost' in the capture's log (the ring overran: the mic
 *      task was starved for 2 s) and the ain task's read errors.
 */
#include "aos_hal.h"
#include "aos_audio.h"
#include "aos_radio.h"
#include "aos_http_stream.h"
#include "aos_audio_p4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "audio";

/* -------------------------------------------------------------------------- */
/* The bus                                                                     */
/* -------------------------------------------------------------------------- */

#define BUS_RATE        48000
#define BUS_BLOCK_MS    10
#define BUS_BLOCK       (BUS_RATE * BUS_BLOCK_MS / 1000)    /* 480 frames */

#define OUT_WARMUP_MS   60          /* silence before the first sound: the amp settles */
#define OUT_IDLE_MS     5000        /* nothing to play this long: the amp goes off */

#define AUDIO_CORE      0           /* LVGL is on 1 */
#define OUT_PRIO        6           /* above LVGL (4) and the workers (5) */
#define IN_PRIO         7           /* the tightest deadline: 30 ms of DMA */
#define MIC_PRIO        6

static esp_codec_dev_handle_t s_speaker;       /* ES8311 */
static esp_codec_dev_handle_t s_mic;           /* ES7210 */
static volatile int  s_hw_state;               /* 0 coming up, 1 up, -1 failed */
static TaskHandle_t  s_aout_task;
static volatile bool s_out_open;               /* the speaker device is open (amp on) */
static volatile bool s_out_for_mic;            /* a capture wants the clock steady */
static volatile bool s_out_mic_ack;            /* ...and the output task is holding it open */
static uint32_t      s_out_opens;              /* for the log: amp cycles */
static volatile uint32_t s_mic_users;          /* the capture's users: recorder, raw microphone */
static int           s_volume = 60;

/* A ring index is published after the data it covers; on this chip's
 * caches a plain volatile would do, but the compiler may still sink the data
 * stores below it. */
#define RING_FENCE()    __atomic_thread_fence(__ATOMIC_SEQ_CST)

static void *big_alloc(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
}

static void bus_fs(esp_codec_dev_sample_info_t *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->bits_per_sample = 16;
    fs->channel         = 2;
    fs->sample_rate     = BUS_RATE;
}

static void aout_kick(void)
{
    TaskHandle_t t = s_aout_task;
    if (t) {
        xTaskNotifyGive(t);
    }
}

static inline int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* ---- resampler (begin) ------------------------------------------------------
 *
 * Polyphase windowed sinc for mono int16 at any pair of rates. A window of
 * 'taps' input samples slides over the input; an output falls between its
 * two middle samples at a fraction 'pos' (Q32), and is the dot product of
 * the window with the row of coefficients for that fraction (256 rows,
 * Blackman-windowed sinc, Q14, each row summing to exactly 1.0 so DC comes
 * out as it went in at every phase).
 *
 * Upsampling cuts at 0.9 of the input's Nyquist with 32 taps; downsampling
 * cuts at 0.9 of the OUTPUT's Nyquist and widens the window by the ratio so
 * the filter keeps its shape (48 -> 16 kHz: 96 taps), up to 128. Equal
 * rates are a copy.
 *
 * Two ways in, one state: rs_process() pushes a block of input and returns
 * what came out (the decoder, the capture); the output task pulls exactly
 * one block of output and consumes what that takes from a ring, stopping
 * where the ring runs dry (the app's stream). Not thread-safe: one owner.
 * -------------------------------------------------------------------------- */

#define RS_PHASE_BITS   8
#define RS_PHASES       (1 << RS_PHASE_BITS)
#define RS_TAPS         32
#define RS_TAPS_MAX     128
#define RS_CUTOFF       0.90f
#define RS_ONE          ((uint64_t)1 << 32)
#define RS_PI           3.14159265358979f

#ifndef RS_ALLOC
#define RS_ALLOC(n)     heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#endif

typedef struct {
    uint32_t in_rate, out_rate;
    int      taps;                  /* 0: equal rates, a copy */
    int16_t *coef;                  /* RS_PHASES rows of 'taps', Q14 */
    int16_t *hist;                  /* 2 * taps: the window, written twice */
    int      hi;                    /* where the oldest sample of the window is */
    uint64_t step;                  /* in / out, Q32 */
    uint64_t pos;                   /* the next output, Q32 from the window's middle */
} rs_t;

static void rs_reset(rs_t *r)
{
    if (r->hist) {
        memset(r->hist, 0, (size_t)2 * (size_t)r->taps * sizeof(int16_t));
    }
    r->hi  = 0;
    r->pos = RS_ONE;                /* one push before the first output */
}

static void rs_build(int16_t *coef, int taps, float fc)
{
    const int m = taps / 2;
    float row[RS_TAPS_MAX];
    for (int p = 0; p < RS_PHASES; p++) {
        float f = (float)p / (float)RS_PHASES;
        float sum = 0.0f;
        for (int k = 0; k < taps; k++) {
            /* where window sample k is, in input samples, from the output */
            float x = (float)(k - (m - 1)) - f;
            float v = 0.0f;
            if (fabsf(x) < (float)m) {
                float a = RS_PI * fc * x;
                float s = fabsf(a) < 1e-6f ? 1.0f : sinf(a) / a;
                float w = 0.42f + 0.5f * cosf(RS_PI * x / (float)m) +
                          0.08f * cosf(2.0f * RS_PI * x / (float)m);
                v = fc * s * w;
            }
            row[k] = v;
            sum += v;
        }
        float scale = sum != 0.0f ? 16384.0f / sum : 0.0f;
        int isum = 0;
        int16_t *c = coef + (size_t)p * (size_t)taps;
        for (int k = 0; k < taps; k++) {
            int q = (int)lrintf(row[k] * scale);
            c[k] = (int16_t)q;
            isum += q;
        }
        /* what rounding lost goes to the tap nearest the output */
        c[(m - 1) + (f >= 0.5f ? 1 : 0)] += (int16_t)(16384 - isum);
    }
}

/* False only without memory (the old configuration stays). A change of
 * rates with the same window keeps the history: gapless between a 44.1 and
 * a 32 kHz track is continuous to the sample. */
static bool rs_config(rs_t *r, uint32_t in, uint32_t out)
{
    if (!in || !out) {
        return false;
    }
    if (r->in_rate == in && r->out_rate == out) {
        return true;
    }
    int taps = 0;
    float fc = RS_CUTOFF;
    if (in != out) {
        taps = RS_TAPS;
        if (out < in) {
            fc = RS_CUTOFF * (float)out / (float)in;
            taps = (int)ceilf((float)RS_TAPS * (float)in / (float)out);
            taps = (taps + 1) & ~1;
            if (taps > RS_TAPS_MAX) {
                taps = RS_TAPS_MAX;
            }
        }
    }
    int16_t *coef = NULL, *hist = r->hist;
    if (taps) {
        coef = RS_ALLOC((size_t)RS_PHASES * (size_t)taps * sizeof(int16_t));
        if (taps != r->taps || !hist) {
            hist = RS_ALLOC((size_t)2 * (size_t)taps * sizeof(int16_t));
        }
        if (!coef || !hist) {
            free(coef);
            if (hist != r->hist) {
                free(hist);
            }
            return false;
        }
        rs_build(coef, taps, fc);
    } else {
        hist = NULL;
    }
    bool keep = taps && taps == r->taps && hist == r->hist;
    free(r->coef);
    if (hist != r->hist) {
        free(r->hist);
    }
    r->coef     = coef;
    r->hist     = hist;
    r->taps     = taps;
    r->in_rate  = in;
    r->out_rate = out;
    r->step     = ((uint64_t)in << 32) / out;
    if (!keep) {
        rs_reset(r);
    }
    return true;
}

/* The most rs_process() can return for 'n' inputs. */
static int rs_max_out(const rs_t *r, int n)
{
    if (!r->taps || !r->in_rate) {
        return n;
    }
    return (int)((uint64_t)n * r->out_rate / r->in_rate) + 2;
}

static inline void rs_push(rs_t *r, int16_t s)
{
    r->hist[r->hi] = s;
    r->hist[r->hi + r->taps] = s;
    r->hi = r->hi + 1 == r->taps ? 0 : r->hi + 1;
}

static inline int16_t rs_eval(const rs_t *r)
{
    uint32_t phase = (uint32_t)(r->pos >> (32 - RS_PHASE_BITS));
    const int16_t *c = r->coef + (size_t)phase * (size_t)r->taps;
    const int16_t *w = r->hist + r->hi;
    int32_t acc = 0;
    for (int k = 0; k < r->taps; k++) {
        acc += (int32_t)c[k] * (int32_t)w[k];
    }
    return sat16((acc + (1 << 13)) >> 14);
}

/* 'n' inputs in, what they give out; 'out' holds rs_max_out(r, n). */
static int rs_process(rs_t *r, const int16_t *in, int n, int16_t *out)
{
    if (!r->taps) {
        if (out != in) {
            memmove(out, in, (size_t)n * sizeof(int16_t));
        }
        return n;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        rs_push(r, in[i]);
        r->pos -= RS_ONE;
        while (r->pos < RS_ONE) {
            out[m++] = rs_eval(r);
            r->pos += r->step;
        }
    }
    return m;
}

/* ---- resampler (end) ------------------------------------------------------ */

/* -------------------------------------------------------------------------- */
/* Tones                                                                       */
/* -------------------------------------------------------------------------- */

/* aos_hal_beep() enqueues and returns straight away (a 60 ms beep used to
 * freeze its caller, which is always an LVGL timer, for 60 ms). The output
 * task synthesises the notes one after another at the bus rate, with the
 * watch's bell: a short attack, an exponential decay and a little third
 * harmonic. It sounds like a bell and not like a buzzer, and there is no
 * click at the cut. */
#define TONE_QUEUE_LEN  16
#define TONE_AMPLITUDE  5500.0f

typedef struct {
    uint16_t freq;
    uint16_t ms;
} tone_note_t;

static QueueHandle_t s_tone_queue;
static tone_note_t   s_tone;                    /* in flight: the output task's */
static uint32_t      s_tone_total, s_tone_done;

static bool tone_pending(void)
{
    return s_tone_done < s_tone_total ||
           (s_tone_queue && uxQueueMessagesWaiting(s_tone_queue) > 0);
}

/* Up to 'n' samples of tone into 'out' (the rest silence); how many sounded. */
static int tone_block(int16_t *out, int n)
{
    int i = 0;
    while (i < n) {
        if (s_tone_done >= s_tone_total) {
            tone_note_t note;
            if (!s_tone_queue || xQueueReceive(s_tone_queue, &note, 0) != pdTRUE) {
                break;
            }
            s_tone       = note;
            s_tone_total = (uint32_t)BUS_RATE * note.ms / 1000;
            s_tone_done  = 0;
            continue;
        }
        const float total = (float)s_tone_total;
        const float w = 2.0f * RS_PI * (float)s_tone.freq / (float)BUS_RATE;
        for (; i < n && s_tone_done < s_tone_total; i++, s_tone_done++) {
            float t = (float)s_tone_done / total;
            float env = (t < 0.04f) ? (t / 0.04f) : expf(-3.5f * (t - 0.04f));
            float ph = w * (float)s_tone_done;
            out[i] = (int16_t)((sinf(ph) + 0.22f * sinf(3.0f * ph)) * env * TONE_AMPLITUDE);
        }
    }
    if (i < n) {
        memset(out + i, 0, (size_t)(n - i) * sizeof(int16_t));
    }
    return i;
}

void aos_hal_beep(int freq_hz, int ms)
{
    if (freq_hz <= 0 || freq_hz > 20000 || ms <= 0 || ms > 2000) {
        return;
    }
    if (!s_tone_queue || s_hw_state < 0 || (s_hw_state == 1 && !s_speaker)) {
        return;             /* the HAL has not started, or there is no speaker */
    }
    tone_note_t note = { (uint16_t)freq_hz, (uint16_t)ms };
    /* if the queue is full the note is lost: better that than stalling the
     * caller, which is nearly always a frame of a game */
    if (xQueueSend(s_tone_queue, &note, 0) == pdTRUE) {
        aout_kick();
    }
}

/* -------------------------------------------------------------------------- */
/* Player                                                                      */
/* -------------------------------------------------------------------------- */

/* Two halves and a ring between them, as on the watch:
 *
 *   player_task   file or station -> aos_audio -> mono -> 48 kHz -> ring
 *   music_block() ring -> the output task's mix, 10 ms at a time
 *
 * The ring is four seconds of mono at the bus rate in PSRAM, and it is what
 * lets the decoder give way: while it is more than three quarters full the
 * decoder runs at priority 2, under LVGL (4), the workers (5) and anything
 * else an app does, and only when it falls under that does it rise to 5,
 * level with the workers, before the writer runs dry. The WRITER moves it (the watch's
 * measured lesson: a decoder that raises itself never runs to do it when
 * both cores are busy above 2). A game that keeps both cores busy for
 * seconds on end is where the music gives way; the writer counts those as
 * underruns, visible in /api/player.
 *
 * Mono because there is one speaker behind a mono DAC: (L+R)/2, as the
 * watch learned (a stereo stream plays one slot of the ES8311, and a track
 * mixed wide loses whatever is only on the right).
 *
 * The music gives way to an app: the streaming speaker (aos_hal_spk_open)
 * and the microphone pause it and it comes back when they are done, without
 * being asked. After a track, the next one of its folder, in name order and
 * round again, or at random with shuffle; this lives here and not in the
 * Music app so the music goes on with the app closed.
 *
 * Every position is in ring samples at the bus rate; s_player_rate is the
 * source's, for what the screen says about the file. */

#define PLAYER_RING_S       4           /* seconds of mono PCM in PSRAM: 384 KB */
#define PLAYER_CHUNK        1152        /* frames per decode: one MPEG-1 frame */
#define PLAYER_PREFILL_MS   200         /* the writer starts (and restarts) with this much */
#define PLAYER_PRIO_LOW     2
#define PLAYER_PRIO_HIGH    5
#define PLAYER_MAX_TRACKS   512
#define PLAYER_MIN_RATE     4000        /* the resampled chunk is sized for this */
#define PLAYER_MAX_RATE     192000

static volatile aos_player_state_t s_player_state;
static volatile bool      s_player_abort;
static volatile bool      s_music_on;           /* the writer's half is running (the watch's out task) */
static bool               s_mus_filling = true; /* the writer waits for PLAYER_PREFILL_MS */
static portMUX_TYPE       s_player_mux = portMUX_INITIALIZER_UNLOCKED;

/* what is being heard (published by player_task under s_player_mux) */
static char             s_player_path[256];
static char             s_player_title[96];
static char             s_player_artist[96];
static char             s_player_album[64];
static aos_audio_info_t s_player_info;
static uint32_t         s_player_rate = 44100;  /* the source's */
static uint8_t          s_player_channels = 2;

/* the folder */
static aos_audio_list_t s_player_list;
static int              s_player_index = -1;     /* -1: a single file */
static bool             s_player_shuffle;

/* requests to player_task */
static volatile int32_t s_player_seek_ms = -1;
static volatile int     s_player_skip;           /* +1 next, -1 previous */

/* the ring: mono samples at BUS_RATE; head and tail count samples forever */
static int16_t          *s_pring;
static uint32_t          s_pring_n;
static volatile uint32_t s_pring_head, s_pring_tail;
static volatile uint32_t s_track_start;          /* tail value at 0:00 of the track heard */
static volatile bool     s_dec_done;             /* nothing more will come */
static volatile bool     s_out_hold;             /* park the writer */
static volatile bool     s_out_parked;
static portMUX_TYPE      s_out_mux = portMUX_INITIALIZER_UNLOCKED;   /* hold + parked */
static volatile bool     s_expect_empty;         /* a flush or the end of a track: not an underrun */
static volatile int      s_yield_refs;           /* the app's speaker, the microphone */
static volatile int64_t  s_yield_until_us;       /* ...and a moment after they let go */
static portMUX_TYPE      s_yield_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     s_player_yielded_pause; /* ...and paused us for it */

/* figures for /api/player */
static volatile uint32_t s_player_underruns;
static volatile uint32_t s_dec_frames, s_dec_us_total, s_dec_us_max;
static volatile uint32_t s_cost_frames;          /* aos_audio_cost(): decoding alone */
static volatile uint64_t s_cost_us;
static volatile uint32_t s_ring_min_ms = UINT32_MAX;
static bool              s_ring_primed;          /* was 3/4 full since the last flush */
static int               s_dec_prio;

static TaskHandle_t      s_player_task;

static void player_title_from(const char *path, const aos_audio_info_t *info)
{
    const char *slash = strrchr(path, '/');
    char name[96];
    snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = '\0';
    }

    /* The tags when there are any; otherwise the file name, which in most
     * collections is "Artist - Title". */
    char title[96] = "", artist[96] = "";
    const char *sep = strstr(name, " - ");
    const char *after = sep ? sep + 3 : NULL;
    while (after && *after == ' ') {
        after++;                        /* "Aman Anand -  Raikou": two spaces */
    }
    if (info->title[0]) {
        snprintf(title, sizeof(title), "%s", info->title);
    } else if (sep) {
        snprintf(title, sizeof(title), "%s", after);
    } else {
        snprintf(title, sizeof(title), "%s", name);
    }
    if (info->artist[0]) {
        snprintf(artist, sizeof(artist), "%s", info->artist);
    } else if (sep && !(info->title[0] && strcmp(info->title, name) != 0)) {
        snprintf(artist, sizeof(artist), "%.*s", (int)(sep - name), name);
        /* a title tag that is just the file name again: split that too */
        if (info->title[0]) {
            snprintf(title, sizeof(title), "%s", after);
        }
    }
    portENTER_CRITICAL(&s_player_mux);
    memcpy(s_player_title, title, sizeof(title));
    memcpy(s_player_artist, artist, sizeof(artist));
    memcpy(s_player_album, info->album, sizeof(s_player_album));
    portEXIT_CRITICAL(&s_player_mux);
}

/* Copies at most 'len' bytes of a UTF-8 string without cutting a character
 * in half (the status's title is 64 bytes; ours, 96). */
static void utf8_copy(char *dst, size_t len, const char *src)
{
    size_t n = strnlen(src, len - 1);
    if (n == len - 1) {
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t ring_used(void)
{
    return s_pring_head - s_pring_tail;
}

static uint32_t ring_ms(uint32_t samples)
{
    return (uint32_t)((uint64_t)samples * 1000 / BUS_RATE);
}

static bool player_parked(void)
{
    portENTER_CRITICAL(&s_out_mux);
    bool parked = s_out_parked;
    portEXIT_CRITICAL(&s_out_mux);
    return parked;
}

/* Parks the writer between two blocks, so the ring can be emptied under it.
 * The hold and the writer's "parked" change under one lock: with two plain
 * flags a skip that came right after another could read a "parked" left over
 * from the last time while the writer had already decided to write, and the
 * tail would end up past the head. No time limit: the writer comes back to
 * the check within a block, or an open of the codec. */
static void player_park(bool park)
{
    portENTER_CRITICAL(&s_out_mux);
    s_out_hold = park;
    portEXIT_CRITICAL(&s_out_mux);
    if (park) {
        aout_kick();
    }
    while (park && s_music_on && !player_parked()) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void player_decoder_prio(void);
static uint32_t player_boundary(uint32_t n);

static bool player_yielded(void)
{
    return s_yield_refs > 0 || esp_timer_get_time() < s_yield_until_us;
}

/* ---- mixing an app's sound over the music ------------------------------------
 *
 * With Settings -> Sound -> "Mix music and apps" on, an app that opens the
 * streaming speaker while music plays does not pause it: the app's stream is
 * added over the music, brought to the bus rate by its resampler, with the
 * music at half volume (MIX_MUSIC_GAIN) for as long as the app holds the
 * speaker. The walkie-talkie is never mixed (a voice over music is not
 * something anyone asked for), nor is anything while the microphone is open.
 * Off, the default: the music pauses. */
#define MIX_MUSIC_GAIN_Q8   128         /* 0.5 */

static int           s_mix_pref = -1;   /* the setting, read once */
static volatile bool s_fg_voice;        /* the app in front is the walkie-talkie */

bool aos_hal_player_mix(void)
{
    if (s_mix_pref < 0) {
        int32_t v = 0;
        aos_hal_pref_get_i32("mus_mix", &v);
        s_mix_pref = v != 0;
    }
    return s_mix_pref == 1;
}

void aos_hal_player_set_mix(bool on)
{
    s_mix_pref = on ? 1 : 0;
    aos_hal_pref_set_i32("mus_mix", on ? 1 : 0);
}

void aos_hal_audio_foreground(const char *app_id)
{
    s_fg_voice = app_id && strcmp(app_id, "aos.walkie") == 0;
}

/* ---- the writer: the music's part of the output task ------------------------ */

static void music_end(void)
{
    portENTER_CRITICAL(&s_out_mux);
    s_out_parked = true;
    portEXIT_CRITICAL(&s_out_mux);
    s_music_on = false;
}

/* Would music_block() give sound now? Asked by the output task while the
 * speaker is closed, without consuming anything. */
static bool music_ready(void)
{
    if (!s_music_on || s_player_abort || s_out_hold ||
        s_player_state != AOS_PLAYER_PLAYING || player_yielded()) {
        return false;
    }
    uint32_t used = ring_used();
    return used >= (uint32_t)BUS_RATE * PLAYER_PREFILL_MS / 1000 || (s_dec_done && used > 0);
}

/* The speaker is closed: nobody reads the ring, so the writer is parked, and
 * a player that ended or was stopped is let go of here. */
static void music_idle(void)
{
    if (!s_music_on) {
        return;
    }
    if (s_player_abort || (s_dec_done && ring_used() == 0)) {
        music_end();
        return;
    }
    portENTER_CRITICAL(&s_out_mux);
    s_out_parked = true;
    portEXIT_CRITICAL(&s_out_mux);
    player_decoder_prio();
}

/* The music of one block, taken from the ring into 'mus'; how many samples.
 * The watch's player_out_task, minus the codec: that is the output task's
 * now. */
static int music_take(int16_t *mus)
{
    if (!s_music_on) {
        return 0;
    }
    if (s_player_abort) {
        music_end();
        return 0;
    }
    bool paused = s_player_state == AOS_PLAYER_PAUSED;
    bool release = paused || player_yielded();
    portENTER_CRITICAL(&s_out_mux);
    bool hold = s_out_hold;
    s_out_parked = release || hold;     /* decided together: see player_park() */
    portEXIT_CRITICAL(&s_out_mux);
    if (release || hold) {
        return 0;
    }
    player_decoder_prio();

    uint32_t used = ring_used();
    if (used == 0) {
        if (s_dec_done) {
            music_end();
            return 0;
        }
        if (!s_mus_filling && !s_expect_empty) {
            s_player_underruns++;
        }
        s_mus_filling = true;
    }
    if (s_mus_filling) {
        if (used < (uint32_t)BUS_RATE * PLAYER_PREFILL_MS / 1000 && !s_dec_done) {
            return 0;
        }
        s_mus_filling = false;
        s_expect_empty = false;
    }
    /* the low-water mark, from the moment the ring was full: the prefill
     * after a start or a seek is not the decoder falling behind */
    if (used > s_pring_n * 3 / 4) {
        s_ring_primed = true;
    } else if (s_expect_empty) {
        s_ring_primed = false;
    }
    uint32_t left_ms = ring_ms(used);
    if (s_ring_primed && left_ms < s_ring_min_ms) {
        s_ring_min_ms = left_ms;
    }

    int got = 0;
    while (got < BUS_BLOCK) {
        /* the title changes on the sample: the block is split at the boundary */
        uint32_t n = player_boundary((uint32_t)(BUS_BLOCK - got));
        uint32_t u = ring_used();
        if (n > u) {
            n = u;
        }
        if (n == 0) {
            break;
        }
        RING_FENCE();
        uint32_t at = s_pring_tail % s_pring_n;
        uint32_t first = n < s_pring_n - at ? n : s_pring_n - at;
        memcpy(mus + got, s_pring + at, first * sizeof(int16_t));
        if (n > first) {
            memcpy(mus + got + first, s_pring, (n - first) * sizeof(int16_t));
        }
        s_pring_tail += n;
        got += (int)n;
    }
    return got;
}

/* One block of music into 'mus', the rest of it silence. */
static int music_block(int16_t *mus)
{
    int got = music_take(mus);
    if (got < BUS_BLOCK) {
        memset(mus + got, 0, (size_t)(BUS_BLOCK - got) * sizeof(int16_t));
    }
    return got;
}

/* Which track follows 'index' in the folder, or -1 at the end of a single
 * file. With shuffle, any other one. */
static int player_next_index(int index, int step)
{
    int count = s_player_list.count;
    if (index < 0 || count == 0) {
        return -1;
    }
    if (s_player_shuffle && count > 1) {
        int r = (int)(esp_random() % (uint32_t)(count - 1));
        return r >= index ? r + 1 : r;
    }
    return ((index + step) % count + count) % count;
}

/* The decoder's state lives here and not on its stack, so that the task can
 * move to the other core and its successor pick up where it left off.
 *
 * Why it moves: minimp3 decodes in float, and the first FPU instruction pins
 * a task to the core it ran it on. On the watch, created unpinned, the
 * decoder landed on the core the Visor 3D worker spun on while the other had
 * room. So it follows the app instead: when a worker starts on one core the
 * decoder moves to the other, and with no worker it sits on core 0, away
 * from LVGL (aos_audio_p4_worker_core()). */
static aos_audio_t     *s_dec;
static aos_audio_info_t s_dec_info;
static char             s_dec_path[256];
static int16_t         *s_dec_chunk;           /* PLAYER_CHUNK stereo frames, PSRAM */
static int16_t         *s_dec_rsbuf;           /* one chunk at the bus rate, PSRAM */
static rs_t             s_dec_rs;              /* source rate -> BUS_RATE */
static int              s_dec_failures;
static int              s_dec_index = -1;      /* the decoder's file in the folder (may be ahead) */
static bool             s_dec_finished;        /* the file ended; its tail is still playing */
static volatile int     s_dec_core_want;
static int              s_worker_core = -1;    /* where the app's worker runs, -1 none */
static SemaphoreHandle_t s_dec_lock;           /* s_player_task changing hands */

#define DEC_RSBUF_N     (PLAYER_CHUNK * (BUS_RATE / PLAYER_MIN_RATE) + 4)

enum { STEP_AGAIN, STEP_WAIT, STEP_DONE };

/* Over three quarters of the ring: the decoder out of everyone's way;
 * under: level with the apps. Called by the writer, which always runs; under
 * s_dec_lock so the handle is never one that just deleted itself.
 *
 * On the board (2026-09-29) the thumbnail loader, at 3 on the same core,
 * held the decoder off for 810 ms at a time while it sat at 2: with a
 * two-second ring rising at half, that left one second of margin, and a
 * folder of photos in Archivos cut the music. Four seconds, rising at three:
 * three seconds of margin. */
static void player_decoder_prio(void)
{
    int want = ring_used() > s_pring_n / 4 * 3 ? PLAYER_PRIO_LOW : PLAYER_PRIO_HIGH;
    if (want == s_dec_prio || !s_dec_lock) {
        return;
    }
    if (xSemaphoreTake(s_dec_lock, 0) == pdTRUE) {
        if (s_player_task) {
            vTaskPrioritySet(s_player_task, (UBaseType_t)want);
            s_dec_prio = want;
        }
        xSemaphoreGive(s_dec_lock);
    }
}

/* Drops what is queued; the resampler starts from silence with what comes
 * next (a seek, a skip, another station). The decoder's side only. */
static void player_drop_queued(void)
{
    player_park(true);
    s_expect_empty = true;
    s_pring_tail = s_pring_head;
    player_park(false);
    rs_reset(&s_dec_rs);
}

/* The decoder's resampler for what it just opened. False for a rate this
 * player does not take (or no memory for the filter). */
static bool player_dec_rate(uint32_t rate)
{
    if (rate < PLAYER_MIN_RATE || rate > PLAYER_MAX_RATE) {
        ESP_LOGW(TAG, "player: %lu Hz is not a rate this player takes", (unsigned long)rate);
        return false;
    }
    if (!rs_config(&s_dec_rs, rate, BUS_RATE)) {
        ESP_LOGE(TAG, "player: no PSRAM for the %lu Hz resampler", (unsigned long)rate);
        return false;
    }
    return true;
}

/* Room for one decoded chunk once it is brought to the bus rate. */
static uint32_t player_chunk_room(void)
{
    return (uint32_t)rs_max_out(&s_dec_rs, PLAYER_CHUNK);
}

/* Makes 'path' (with its info and folder index) the track that is heard,
 * starting at ring position 'start'. */
static void player_publish(const char *path, const aos_audio_info_t *info, int index,
                           uint32_t start)
{
    portENTER_CRITICAL(&s_player_mux);
    memcpy(s_player_path, path, sizeof(s_player_path));
    s_player_info = *info;
    portEXIT_CRITICAL(&s_player_mux);
    player_title_from(path, info);
    s_player_index    = index;
    s_player_rate     = info->sample_rate;
    s_player_channels = info->channels;
    s_track_start     = start;
}

/* The next track, already decoding behind the one being heard; the writer
 * publishes it when the tail reaches s_next_at (gapless). */
static char             s_next_path[256];
static aos_audio_info_t s_next_info;
static int              s_next_index;
static uint32_t         s_next_at;
static volatile bool    s_next_pending;

/* Called by the writer before every piece of a block: at the boundary, the
 * next track becomes the one heard. Returns how many samples may be taken
 * before it. */
static uint32_t player_boundary(uint32_t n)
{
    if (!s_next_pending) {
        return n;
    }
    uint32_t left = s_next_at - s_pring_tail;
    if ((int32_t)left <= 0) {
        player_publish(s_next_path, &s_next_info, s_next_index, s_next_at);
        s_next_pending = false;
        return n;
    }
    return n < left ? n : left;         /* the title changes on the sample */
}

/* How the next file opened becomes the one heard. With one bus rate for
 * everything, a track at another rate is gapless too: the watch's third way
 * (drain the ring, reopen the codec) is gone. */
enum { PUB_NOW, PUB_AT_HEAD };
static int s_dec_publish = PUB_NOW;

/* Closes the decoder's file and points it at the track 'step' away from
 * 'from'. */
static int player_point_at(int from, int step, int publish)
{
    aos_audio_close(s_dec);
    s_dec = NULL;
    int next = player_next_index(from, step);
    if (next < 0) {
        return STEP_DONE;               /* a single file: over */
    }
    s_dec_index = next;
    s_dec_publish = publish;
    snprintf(s_dec_path, sizeof(s_dec_path), "%s/%s", s_player_list.dir,
             aos_audio_list_name(&s_player_list, next));
    return STEP_AGAIN;
}

/* A skip or a seek acts on what is HEARD. If the decoder is already into the
 * next track (the last two seconds of this one are in the ring), it goes
 * back to the one heard first. */
static void player_back_to_heard(void)
{
    if (!s_next_pending) {
        return;
    }
    s_next_pending = false;             /* the writer is parked: see callers */
    aos_audio_close(s_dec);
    s_dec = NULL;
    portENTER_CRITICAL(&s_player_mux);
    memcpy(s_dec_path, s_player_path, sizeof(s_dec_path));
    portEXIT_CRITICAL(&s_player_mux);
    s_dec_index = s_player_index;
    s_dec_publish = PUB_NOW;
}

/* ---- internet radio ---------------------------------------------------------
 *
 * A station is one more source for the same decoder, ring and writer:
 * s_dec_path holds its URL, aos_radio.c keeps the connection and
 * aos_audio_open_src() decodes out of its ring. What is different from a
 * file, all of it in radio_step():
 *
 *  - Opening waits for the ring to hold a second and a half WITHOUT blocking
 *    the task: a stop or a skip during a ten-second TLS handshake is heard
 *    at once, and the reader is left to give up on its own.
 *  - A dry ring is a wait, not the end of the track; the end is the reader
 *    giving up (aos_audio_ended()).
 *  - Next and previous move along the list of stations the app gave.
 *  - The title follows the stream's StreamTitle, and it is published when
 *    its audio reaches the speaker: the metadata arrives with the bytes, and
 *    those are heard two rings later (up to ~14 s at 128 kbps).
 *  - A pause longer than the rings can hold resumes live: the listener
 *    expects the radio, not a recording of what it said a while ago.
 * -------------------------------------------------------------------------- */
#define RADIO_LIVE_AFTER_MS 20000

static bool                 s_radio_mode;
static aos_radio_station_t *s_radio_list;           /* AOS_RADIO_MAX_STATIONS, PSRAM */
static int                  s_radio_count;
static volatile int         s_radio_index;
static int64_t              s_radio_started_ms;
static bool                 s_radio_open;           /* aos_radio_start() done for s_dec_path */
static uint32_t             s_radio_seen_gen;       /* the last title the decoder met */
static char                 s_radio_pend_title[128];
static uint32_t             s_radio_pend_at;        /* ring sample where it is heard */
static volatile bool        s_radio_pend;
static char                 s_radio_heard[128];     /* under s_player_mux */
static volatile uint32_t    s_radio_heard_gen;
static int64_t              s_radio_paused_ms;
static int                  s_radio_ps;             /* the radio's vote on the WiFi's power save */

/* The heard title, split into artist and title the way stations write it:
 * "Artist - Title". */
static void radio_set_heard(const char *full)
{
    char title[96], artist[96] = "";
    const char *sep = strstr(full, " - ");
    if (sep) {
        snprintf(artist, sizeof(artist), "%.*s", (int)(sep - full), full);
        snprintf(title, sizeof(title), "%s", sep + 3);
    } else {
        snprintf(title, sizeof(title), "%s", full);
    }
    portENTER_CRITICAL(&s_player_mux);
    memcpy(s_player_title, title, sizeof(title));
    memcpy(s_player_artist, artist, sizeof(artist));
    snprintf(s_radio_heard, sizeof(s_radio_heard), "%s", full);
    s_radio_heard_gen++;
    portEXIT_CRITICAL(&s_player_mux);
}

/* The station that is now the one heard: its name as the album, no title
 * yet, the format unknown until the first frame. */
static void radio_publish_station(void)
{
    int i = s_radio_index;
    if (!s_radio_list || i < 0 || i >= s_radio_count) {
        return;
    }
    aos_audio_info_t none = {0};
    s_radio_pend = false;
    s_radio_seen_gen = 0;
    portENTER_CRITICAL(&s_player_mux);
    memcpy(s_player_path, s_radio_list[i].url, sizeof(s_radio_list[i].url));
    s_player_info = none;
    memcpy(s_player_album, s_radio_list[i].name, sizeof(s_radio_list[i].name));
    s_player_album[sizeof(s_player_album) - 1] = '\0';
    portEXIT_CRITICAL(&s_player_mux);
    radio_set_heard("");
    s_player_index = i;
}

/* The next station with an address, 'step' away. */
static int radio_next_index(int from, int step)
{
    for (int k = 1; k <= s_radio_count; k++) {
        int i = ((from + step * k) % s_radio_count + s_radio_count) % s_radio_count;
        if (s_radio_list[i].url[0]) {
            return i;
        }
    }
    return from;
}

/* Drops what is queued and the connection; with 'step', moves along the
 * list. The decoder opens the station again on its next step. */
static void radio_restart(int step)
{
    player_drop_queued();
    aos_audio_close(s_dec);
    s_dec = NULL;
    aos_radio_stop();
    s_radio_open = false;
    if (step) {
        s_radio_index = radio_next_index(s_radio_index, step);
    }
    snprintf(s_dec_path, sizeof(s_dec_path), "%s", s_radio_list[s_radio_index].url);
    radio_publish_station();
    s_radio_started_ms = (int64_t)aos_hal_uptime_ms();
}

static void ring_put_chunk(int n);

/* The radio's say in the WiFi's power save: 2 while it connects, reconnects
 * or fetches an HLS segment (a TLS handshake is a dozen round trips, and
 * modem sleep makes each one 200-300 ms), else nothing. On this board the
 * WiFi otherwise sits in WIFI_PS_MIN_MODEM, which a stream is fine with (the
 * watch's deepest modem sleep does not exist here), so "1, playing" asks for
 * nothing. aos_hal_net_low_latency() is a plain switch and not a count: an
 * app that holds it (a camera) while a station reconnects gets it dropped
 * when the radio lets go. Rare enough to leave for when the power policy
 * gets its votes, like the watch's pm_policy_apply(). */
static void radio_ps(int want)
{
    if (want == s_radio_ps) {
        return;
    }
    bool was = s_radio_ps == 2, now = want == 2;
    s_radio_ps = want;
    if (was != now) {
        aos_hal_net_low_latency(now);
    }
}

static int radio_step(void)
{
    /* the title whose audio has reached the speaker */
    if (s_radio_pend && (int32_t)(s_pring_tail - s_radio_pend_at) >= 0) {
        s_radio_pend = false;
        radio_set_heard(s_radio_pend_title);
    }

    int64_t now = (int64_t)aos_hal_uptime_ms();
    if (s_player_state == AOS_PLAYER_PAUSED) {
        if (!s_radio_paused_ms) {
            s_radio_paused_ms = now;
        }
    } else if (s_radio_paused_ms) {
        bool stale = now - s_radio_paused_ms > RADIO_LIVE_AFTER_MS;
        s_radio_paused_ms = 0;
        if (stale) {
            radio_restart(0);
            return STEP_AGAIN;
        }
    }

    int skip = s_player_skip;
    if (skip) {
        s_player_skip = 0;
        radio_restart(skip);
        return STEP_AGAIN;
    }
    s_player_seek_ms = -1;              /* nothing to seek in a live stream */

    if (!s_dec) {
        radio_ps(2);
        if (!s_radio_open) {
            if (!aos_radio_start(s_dec_path)) {
                return STEP_DONE;
            }
            s_radio_open = true;
        }
        int ready = aos_radio_ready();
        if (ready == 0) {
            s_expect_empty = true;      /* connecting is not an underrun */
            return STEP_WAIT;
        }
        if (ready < 0) {
            return STEP_DONE;           /* the reason stays in the radio's status */
        }
        s_dec = aos_audio_open_src(aos_radio_read, NULL, &s_dec_info);
        if (!s_dec) {
            ESP_LOGW(TAG, "radio: no MP3 or AAC frames in %s", s_dec_path);
            aos_radio_fail("no MP3 or AAC audio in the stream");
            return STEP_DONE;
        }
        if (!player_dec_rate(s_dec_info.sample_rate)) {
            aos_radio_fail("a sample rate this player does not take");
            return STEP_DONE;
        }
        s_dec_frames = 0;
        s_dec_us_total = 0;
        s_cost_frames = 0;
        s_cost_us = 0;
        portENTER_CRITICAL(&s_player_mux);
        s_player_info = s_dec_info;
        portEXIT_CRITICAL(&s_player_mux);
        s_player_rate = s_dec_info.sample_rate;
        s_player_channels = s_dec_info.channels;
        s_track_start = s_pring_tail;
        radio_ps(1);
        ESP_LOGI(TAG, "radio: %s, %s %lu Hz %u ch, %u kbps", s_dec_path, s_dec_info.codec,
                 (unsigned long)s_dec_info.sample_rate, (unsigned)s_dec_info.channels,
                 (unsigned)s_dec_info.kbps);
    }

    radio_ps(aos_radio_busy() ? 2 : 1);
    if (s_pring_n - ring_used() < player_chunk_room()) {
        return STEP_WAIT;
    }
    int64_t t0 = esp_timer_get_time();
    int n = aos_audio_read(s_dec, s_dec_chunk, PLAYER_CHUNK);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (n <= 0) {
        return aos_audio_ended(s_dec) ? STEP_DONE : STEP_WAIT;
    }
    s_dec_frames += (uint32_t)n;
    s_dec_us_total += us;
    if (us > s_dec_us_max) {
        s_dec_us_max = us;
    }
    uint32_t cf;
    uint64_t cu;
    aos_audio_cost(s_dec, &cf, &cu);
    s_cost_frames = cf;
    s_cost_us = cu;
    ring_put_chunk(n);

    char title[128];
    uint32_t gen = aos_radio_title_at_read(title, sizeof(title));
    if (gen != s_radio_seen_gen) {
        s_radio_seen_gen = gen;
        if (s_radio_pend) {
            radio_set_heard(s_radio_pend_title);    /* two in a row: the older one is late */
        }
        memcpy(s_radio_pend_title, title, sizeof(title));
        s_radio_pend_at = s_pring_head;
        s_radio_pend = true;
    }
    return STEP_AGAIN;
}

/* The decoded chunk, mixed to mono and brought to the bus rate, into the
 * ring. The caller made sure there is player_chunk_room(). */
static void ring_put_chunk(int n)
{
    int16_t *c = s_dec_chunk;
    if (s_dec_info.channels == 2) {
        for (int i = 0; i < n; i++) {
            c[i] = (int16_t)(((int32_t)c[2 * i] + c[2 * i + 1]) / 2);
        }
    }
    int m = rs_process(&s_dec_rs, c, n, s_dec_rsbuf);
    uint32_t head = s_pring_head;
    uint32_t at = head % s_pring_n;
    uint32_t first = (uint32_t)m < s_pring_n - at ? (uint32_t)m : s_pring_n - at;
    memcpy(s_pring + at, s_dec_rsbuf, first * sizeof(int16_t));
    if ((uint32_t)m > first) {
        memcpy(s_pring, s_dec_rsbuf + first, ((uint32_t)m - first) * sizeof(int16_t));
    }
    RING_FENCE();
    s_pring_head = head + (uint32_t)m;
}

static int player_step(void)
{
    if (s_radio_mode) {
        return radio_step();
    }
    if (!s_dec) {
        s_dec = aos_audio_open(s_dec_path, &s_dec_info);
        if (s_dec && !player_dec_rate(s_dec_info.sample_rate)) {
            aos_audio_close(s_dec);
            s_dec = NULL;
        }
        if (!s_dec) {
            ESP_LOGW(TAG, "player: cannot play %s", s_dec_path);
            /* a folder with one bad file goes on to the next; a folder of
             * nothing but bad files, or a single file, stops */
            if (++s_dec_failures >= (s_player_list.count ? s_player_list.count : 1)) {
                return STEP_DONE;
            }
            return player_point_at(s_dec_index, 1, s_dec_publish);
        }
        s_dec_failures = 0;
        s_dec_finished = false;
        s_dec_frames = 0;
        s_dec_us_total = 0;
        s_cost_frames = 0;
        s_cost_us = 0;
        if (s_dec_publish == PUB_AT_HEAD) {
            portENTER_CRITICAL(&s_player_mux);
            memcpy(s_next_path, s_dec_path, sizeof(s_next_path));
            s_next_info  = s_dec_info;
            s_next_index = s_dec_index;
            s_next_at    = s_pring_head;
            s_next_pending = true;
            portEXIT_CRITICAL(&s_player_mux);
        } else {
            /* the ring is empty: first track, or after a flush */
            player_publish(s_dec_path, &s_dec_info, s_dec_index, s_pring_tail);
        }
    }

    int skip = s_player_skip;
    if (skip) {
        s_player_skip = 0;
        player_park(true);
        s_expect_empty = true;
        s_pring_tail = s_pring_head;
        s_next_pending = false;
        player_park(false);
        rs_reset(&s_dec_rs);
        return player_point_at(s_player_index, skip, PUB_NOW);
    }
    int32_t seek = s_player_seek_ms;
    if (seek >= 0) {
        if (s_next_pending) {
            player_park(true);
            s_expect_empty = true;
            s_pring_tail = s_pring_head;
            player_back_to_heard();
            player_park(false);
            rs_reset(&s_dec_rs);
            return STEP_AGAIN;          /* reopen what is heard; the seek waits */
        }
        s_player_seek_ms = -1;
        player_drop_queued();
        uint32_t landed = aos_audio_seek(s_dec, (uint32_t)seek);
        s_track_start = s_pring_tail - (uint32_t)((uint64_t)landed * BUS_RATE / 1000);
        s_dec_finished = false;
    }

    if (s_dec_finished) {
        /* a single file: let the writer play the tail out, then stop */
        if (ring_used() > 0) {
            return STEP_WAIT;
        }
        return STEP_DONE;
    }

    if (s_pring_n - ring_used() < player_chunk_room()) {
        return STEP_WAIT;
    }

    int64_t t0 = esp_timer_get_time();
    int n = aos_audio_read(s_dec, s_dec_chunk, PLAYER_CHUNK);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (n <= 0) {
        if (s_dec_index < 0) {
            s_dec_finished = true;
            s_expect_empty = true;      /* the silence that follows is the end */
            return STEP_AGAIN;
        }
        if (s_next_pending) {
            return STEP_WAIT;           /* a track shorter than the ring: one at a time */
        }
        /* Gapless: the next file starts right behind this one in the ring. */
        return player_point_at(s_dec_index, 1, PUB_AT_HEAD);
    }
    s_dec_frames += (uint32_t)n;
    s_dec_us_total += us;
    if (us > s_dec_us_max) {
        s_dec_us_max = us;
    }
    uint32_t cf;
    uint64_t cu;
    aos_audio_cost(s_dec, &cf, &cu);
    s_cost_frames = cf;
    s_cost_us = cu;
    ring_put_chunk(n);
    return STEP_AGAIN;
}

/* ---- remembering the last track --------------------------------------------
 *
 * The folder track heard and where it was, in NVS: the path when a new track
 * starts, the position every minute, on pause and on stop. After a restart
 * the Music app offers to go on from there (nothing plays by itself). NVS
 * skips a write whose value did not change, so a paused track costs nothing.
 * Single files (a video's sound) are not remembered. On this board the tick
 * runs in the decoder's loop, which is alive for as long as there is a
 * track (the watch used its housekeeping task, which P4OS does not have). */
#define PLAYER_REMEMBER_MS  60000

static char    s_saved_path[256];
static int64_t s_saved_at_ms;

static void player_remember(bool force)
{
    if (s_player_index < 0 || !s_player_task || s_radio_mode) {
        return;
    }
    if (s_player_seek_ms >= 0) {
        return;     /* resume_last's seek not applied yet: 0:00 is not where it is */
    }
    char path[256];
    portENTER_CRITICAL(&s_player_mux);
    memcpy(path, s_player_path, sizeof(path));
    portEXIT_CRITICAL(&s_player_mux);
    int64_t now = (int64_t)aos_hal_uptime_ms();
    bool new_track = strcmp(path, s_saved_path) != 0;
    if (new_track) {
        aos_hal_pref_set_str("mus_path", path);
        memcpy(s_saved_path, path, sizeof(s_saved_path));
    }
    if (new_track || force || now - s_saved_at_ms >= PLAYER_REMEMBER_MS) {
        uint32_t pos = ring_ms(s_pring_tail - s_track_start);
        aos_hal_pref_set_i32("mus_pos", (int32_t)pos);
        s_saved_at_ms = now;
    }
}

static void player_remember_tick(void)
{
    static aos_player_state_t last;
    aos_player_state_t st = s_player_state;
    if (s_player_task && (st == AOS_PLAYER_PLAYING || st != last)) {
        player_remember(st != last);
    }
    last = st;
}

static void player_task(void *arg);

/* A decoder pinned to 'core', made the current one. Under s_dec_lock. */
static bool player_spawn_decoder(int core)
{
    TaskHandle_t t = NULL;
    /* 6 KB: the FAT path of an open plus minimp3 used 3.3 KB on the watch.
     * Internal: it reads the card and writes NVS (remembering the track). */
    if (xTaskCreatePinnedToCore(player_task, "aos_player", 6144, (void *)(intptr_t)core,
                                (UBaseType_t)s_dec_prio, &t, core) != pdPASS) {
        return false;
    }
    s_player_task = t;
    return true;
}

static void player_task(void *arg)
{
    int core = (int)(intptr_t)arg;
    int64_t busy_since = esp_timer_get_time();

    while (!s_player_abort) {
        int want = s_dec_core_want;
        if (want != core) {
            xSemaphoreTake(s_dec_lock, portMAX_DELAY);
            bool moved = player_spawn_decoder(want);
            xSemaphoreGive(s_dec_lock);
            if (moved) {
                ESP_LOGI(TAG, "player: decoder moved to core %d", want);
                vTaskDelete(NULL);      /* the successor carries on */
                return;
            }
            s_dec_core_want = core;     /* no room for a second stack: stay */
        }
        player_remember_tick();
        int r = player_step();
        if (r == STEP_DONE) {
            break;
        }
        if (r == STEP_WAIT) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            busy_since = esp_timer_get_time();
        } else if (esp_timer_get_time() - busy_since > 100000) {
            /* A decoder with work that never runs out must still let its
             * core's idle task run: at priority 5 and slowed down by an OTA
             * writing the flash, it decoded for seconds on end and the task
             * watchdog reset the watch in the middle of the upload
             * (2026-09-26). A tick off every 100 ms costs 1 % and the ring
             * never notices. */
            vTaskDelay(1);
            busy_since = esp_timer_get_time();
        }
    }

    aos_audio_close(s_dec);
    s_dec = NULL;
    if (s_radio_open) {
        aos_radio_stop();
        s_radio_open = false;
    }
    radio_ps(0);
    /* The writer plays out what is left (paused, it waits) and ends on its
     * own; a stop gets there sooner through s_player_abort. */
    s_dec_done = true;
    aout_kick();
    while (s_music_on) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_track_start = s_pring_tail;
    s_player_state = AOS_PLAYER_STOPPED;
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    s_player_task = NULL;
    xSemaphoreGive(s_dec_lock);
    vTaskDelete(NULL);
}

void aos_audio_p4_worker_core(int core)
{
    s_worker_core = core;
    s_dec_core_want = core == 0 ? 1 : AUDIO_CORE;
    TaskHandle_t t = s_player_task;
    if (t) {
        xTaskNotifyGive(t);             /* it moves at the top of its loop */
    }
}

static bool player_start(const char *path)
{
    if (!path || s_hw_state != 1 || !s_speaker) {
        return false;
    }

    if (!s_pring) {
        s_pring_n = BUS_RATE * PLAYER_RING_S;
        s_pring = big_alloc(s_pring_n * sizeof(int16_t));
        if (!s_pring) {
            ESP_LOGE(TAG, "player: no PSRAM for the ring");
            return false;
        }
    }
    if (!s_dec_chunk) {
        s_dec_chunk = big_alloc(PLAYER_CHUNK * 2 * sizeof(int16_t));
    }
    if (!s_dec_rsbuf) {
        s_dec_rsbuf = big_alloc(DEC_RSBUF_N * sizeof(int16_t));
    }
    if (!s_dec_lock) {
        s_dec_lock = xSemaphoreCreateMutex();
    }
    if (!s_dec_chunk || !s_dec_rsbuf || !s_dec_lock) {
        return false;
    }

    portENTER_CRITICAL(&s_player_mux);
    snprintf(s_player_path, sizeof(s_player_path), "%s", path);
    memset(&s_player_info, 0, sizeof(s_player_info));  /* not the last track's length */
    portEXIT_CRITICAL(&s_player_mux);
    aos_audio_info_t none = {0};
    player_title_from(path, &none);
    snprintf(s_dec_path, sizeof(s_dec_path), "%s", path);
    s_dec = NULL;
    s_dec_failures = 0;
    s_dec_index = s_player_index;       /* play()/play_folder() set it just before */
    s_dec_publish = PUB_NOW;
    s_next_pending = false;
    s_dec_finished = false;
    rs_reset(&s_dec_rs);

    s_pring_head = s_pring_tail = s_track_start = 0;
    s_player_abort = false;
    s_player_skip = 0;
    s_player_seek_ms = -1;
    s_dec_done = false;
    s_out_hold = false;
    s_out_parked = false;
    s_dec_us_max = 0;
    s_ring_min_ms = UINT32_MAX;
    s_ring_primed = false;
    s_player_underruns = 0;
    s_expect_empty = true;
    s_dec_prio = PLAYER_PRIO_HIGH;          /* what it is created with */
    /* Started while an app holds the speaker or the microphone: it starts
     * paused, as if it had been playing when the app came, and comes back
     * with the rest when the app lets go. */
    s_player_yielded_pause = s_yield_refs > 0;
    s_player_state = s_player_yielded_pause ? AOS_PLAYER_PAUSED : AOS_PLAYER_PLAYING;

    s_mus_filling = true;
    RING_FENCE();                       /* all of the above before the writer sees it on */
    s_music_on = true;                  /* the writer's half: the output task */
    aout_kick();

    bool started;
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    s_dec_core_want = s_worker_core == 0 ? 1 : AUDIO_CORE;
    started = player_spawn_decoder(s_dec_core_want);
    xSemaphoreGive(s_dec_lock);
    if (!started) {
        s_player_abort = true;
        aout_kick();
        s_player_state = AOS_PLAYER_STOPPED;
        return false;
    }
    return true;
}

bool aos_hal_player_play(const char *path)
{
    aos_hal_player_stop();
    s_radio_mode = false;
    s_player_index = -1;                /* one file: the video's sound, a ringtone */
    aos_audio_list_free(&s_player_list);
    return player_start(path);
}

bool aos_hal_player_play_folder(const char *path)
{
    static bool shuffle_loaded;
    if (!shuffle_loaded) {
        int32_t v = 0;
        aos_hal_pref_get_i32("mus_shuf", &v);
        s_player_shuffle = v != 0;
        shuffle_loaded = true;
    }
    aos_hal_player_stop();
    s_radio_mode = false;
    if (!path) {
        return false;
    }
    char dir[160];
    const char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= sizeof(dir)) {
        return false;
    }
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    aos_audio_list_scan(&s_player_list, dir, PLAYER_MAX_TRACKS);
    s_player_index = aos_audio_list_find(&s_player_list, slash + 1);
    if (s_player_index < 0) {
        aos_audio_list_free(&s_player_list);
    }
    return player_start(path);
}

bool aos_hal_player_last(char *path, size_t len, uint32_t *position_ms)
{
    int32_t pos = 0;
    if (!path || len == 0 || !aos_hal_pref_get_str("mus_path", path, len) || !path[0]) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;                   /* deleted, or the card is out */
    }
    aos_hal_pref_get_i32("mus_pos", &pos);
    if (position_ms) {
        *position_ms = pos > 0 ? (uint32_t)pos : 0;
    }
    return true;
}

bool aos_hal_player_resume_last(void)
{
    char path[256];
    uint32_t pos = 0;
    if (!aos_hal_player_last(path, sizeof(path), &pos) || !aos_hal_player_play_folder(path)) {
        return false;
    }
    if (pos > 0) {
        aos_hal_player_seek(pos);       /* the decoder opens the file, then seeks */
    }
    return true;
}

void aos_hal_player_pause(void)
{
    if (s_player_state == AOS_PLAYER_PLAYING) {
        s_player_state = AOS_PLAYER_PAUSED;
        s_player_yielded_pause = false;
    }
}

void aos_hal_player_resume(void)
{
    /* not while an app has the speaker or the microphone */
    if (s_player_state == AOS_PLAYER_PAUSED && !s_yield_refs) {
        s_player_state = AOS_PLAYER_PLAYING;
        aout_kick();
    }
}

void aos_hal_player_stop(void)
{
    player_remember(true);              /* where it was, before it is gone */
    if (!s_player_task) {
        s_player_state = AOS_PLAYER_STOPPED;
        return;
    }
    s_player_abort = true;
    aout_kick();
    for (int i = 0; i < 100 && (s_player_task || s_music_on); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_player_state = AOS_PLAYER_STOPPED;
}

static bool radio_step_stopped(int step)
{
    if (!s_radio_mode || s_player_task || !s_radio_list || s_radio_count <= 0) {
        return false;
    }
    /* A station that failed or was stopped: next and previous start the
     * neighbour straight away. */
    aos_hal_radio_play(s_radio_list, s_radio_count, radio_next_index(s_radio_index, step));
    return true;
}

void aos_hal_player_next(void)
{
    if (radio_step_stopped(1)) {
        return;
    }
    if (s_player_task) {
        s_player_skip = 1;
        if (s_player_state == AOS_PLAYER_PAUSED && !s_yield_refs) {
            s_player_state = AOS_PLAYER_PLAYING;
        }
        TaskHandle_t t = s_player_task;
        if (t) {
            xTaskNotifyGive(t);
        }
        aout_kick();
    }
}

void aos_hal_player_prev(void)
{
    if (radio_step_stopped(-1) || !s_player_task) {
        return;
    }
    /* like every player: back to the start first, then the previous one */
    uint32_t pos_ms = ring_ms(s_pring_tail - s_track_start);
    if (!s_radio_mode && (pos_ms > 3000 || s_player_index < 0)) {
        s_player_seek_ms = 0;
    } else {
        s_player_skip = -1;
    }
    if (s_player_state == AOS_PLAYER_PAUSED && !s_yield_refs) {
        s_player_state = AOS_PLAYER_PLAYING;
    }
    TaskHandle_t t = s_player_task;
    if (t) {
        xTaskNotifyGive(t);
    }
    aout_kick();
}

void aos_hal_player_seek(uint32_t ms)
{
    if (s_player_task) {
        s_player_seek_ms = (int32_t)ms;
    }
}

void aos_hal_player_set_shuffle(bool on)
{
    s_player_shuffle = on;
    aos_hal_pref_set_i32("mus_shuf", on ? 1 : 0);
}

bool aos_hal_player_status(aos_player_status_t *out)
{
    if (!out) {
        return false;
    }
    out->state       = s_player_state;
    out->duration_s  = s_player_info.duration_ms / 1000;
    out->position_s  = s_player_task ? ring_ms(s_pring_tail - s_track_start) / 1000 : 0;
    out->sample_rate = s_player_rate;
    out->channels    = s_player_channels;
    portENTER_CRITICAL(&s_player_mux);
    memcpy(out->path, s_player_path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    portEXIT_CRITICAL(&s_player_mux);
    char title[96];
    portENTER_CRITICAL(&s_player_mux);
    memcpy(title, s_player_title, sizeof(title));
    portEXIT_CRITICAL(&s_player_mux);
    utf8_copy(out->title, sizeof(out->title), title);
    return true;
}

/* A literal for the format: the info struct is a copy on the stack. */
static const char *codec_name(const aos_audio_info_t *info)
{
    if (info->format == AOS_AUDIO_AAC) {
        return !strcmp(info->codec, "HE-AACv2") ? "HE-AACv2"
             : !strcmp(info->codec, "HE-AAC") ? "HE-AAC" : "AAC";
    }
    return info->format == AOS_AUDIO_MP3 ? "MP3" : (info->format == AOS_AUDIO_WAV ? "WAV" : "");
}

bool aos_hal_player_info(aos_player_info_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_player_mux);
    memcpy(out->path, s_player_path, sizeof(out->path));
    memcpy(out->title, s_player_title, sizeof(out->title));
    memcpy(out->artist, s_player_artist, sizeof(out->artist));
    memcpy(out->album, s_player_album, sizeof(out->album));
    aos_audio_info_t info = s_player_info;
    portEXIT_CRITICAL(&s_player_mux);

    out->state       = s_player_state;
    out->format      = codec_name(&info);
    out->kbps        = info.kbps;
    out->vbr         = info.vbr;
    out->sample_rate = info.sample_rate;
    out->channels    = info.channels;
    out->duration_ms = info.duration_ms;
    out->position_ms = s_player_task ? ring_ms(s_pring_tail - s_track_start) : 0;
    out->index       = s_player_index;
    out->count       = s_player_index >= 0 ? s_player_list.count : 0;
    out->shuffle     = s_player_shuffle;
    out->yielded     = s_yield_refs > 0 && s_player_yielded_pause;
    out->has_cover   = info.cover_offset != 0;
    out->cover_offset = info.cover_offset;
    out->cover_size  = info.cover_size;
    if (s_radio_mode) {
        out->live        = true;
        out->duration_ms = 0;
        out->index       = s_radio_index;
        out->count       = s_radio_count;
        out->shuffle     = false;
        out->has_cover   = false;
    }
    return true;
}

bool aos_hal_radio_play(const aos_radio_station_t *list, int count, int index)
{
    if (!list || count <= 0 || index < 0 || index >= count || !list[index].url[0]) {
        return false;
    }
    if (count > AOS_RADIO_MAX_STATIONS) {
        count = AOS_RADIO_MAX_STATIONS;
        if (index >= count) {
            return false;
        }
    }
    if (!s_radio_list) {
        s_radio_list = big_alloc(AOS_RADIO_MAX_STATIONS * sizeof(aos_radio_station_t));
        if (!s_radio_list) {
            return false;
        }
    }
    aos_http_stream_init();             /* aos_http.c's lock, from this side */
    aos_hal_player_stop();
    if (list != s_radio_list) {
        memcpy(s_radio_list, list, (size_t)count * sizeof(aos_radio_station_t));
    }
    for (int i = 0; i < count; i++) {
        s_radio_list[i].name[sizeof(s_radio_list[i].name) - 1] = '\0';
        s_radio_list[i].url[sizeof(s_radio_list[i].url) - 1] = '\0';
    }
    s_radio_count = count;
    s_radio_index = index;
    aos_audio_list_free(&s_player_list);
    s_player_index = index;
    s_radio_mode = true;
    s_radio_open = false;
    s_radio_paused_ms = 0;
    s_radio_started_ms = (int64_t)aos_hal_uptime_ms();
    char url[256];
    memcpy(url, s_radio_list[index].url, sizeof(url));
    if (!player_start(url)) {
        return false;
    }
    radio_publish_station();
    return true;
}

bool aos_hal_radio_active(void)
{
    return s_radio_mode && s_player_task != NULL;
}

bool aos_hal_radio_status(aos_radio_status_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->index = -1;
    if (!s_radio_mode || !s_radio_list) {
        return true;
    }
    aos_radio_fill_status(out);
    if (!s_player_task && out->state != AOS_RADIO_FAILED) {
        out->state = AOS_RADIO_OFF;
    }
    int i = s_radio_index;
    out->index = i;
    out->count = s_radio_count;
    if (i >= 0 && i < s_radio_count) {
        memcpy(out->station, s_radio_list[i].name, sizeof(out->station));
        memcpy(out->url, s_radio_list[i].url, sizeof(out->url));
    }
    portENTER_CRITICAL(&s_player_mux);
    memcpy(out->title, s_radio_heard, sizeof(out->title));
    aos_audio_info_t info = s_player_info;
    portEXIT_CRITICAL(&s_player_mux);
    out->title_gen   = s_radio_heard_gen;
    snprintf(out->codec, sizeof(out->codec), "%s", codec_name(&info));
    out->sample_rate = info.sample_rate;
    out->channels    = info.channels;
    if (!out->kbps) {
        out->kbps = info.kbps;
    }
    if (s_player_task) {
        out->buffer_ms += ring_ms(ring_used());
        out->listening_s = (uint32_t)(((int64_t)aos_hal_uptime_ms() - s_radio_started_ms) / 1000);
    }
    return true;
}

bool aos_hal_player_stats(aos_player_stats_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    /* decoder time per second of audio, in thousandths of a core: the frames
     * counted are the source's, at the source's rate */
    uint32_t rate = s_player_rate ? s_player_rate : 44100;
    uint32_t frames = s_dec_frames, us = s_dec_us_total;
    out->ring_ms      = s_player_task ? ring_ms(ring_used()) : 0;
    out->ring_cap_ms  = ring_ms(s_pring_n);
    out->ring_min_ms  = s_ring_min_ms == UINT32_MAX ? 0 : s_ring_min_ms;
    out->underruns    = s_player_underruns;
    out->load_permille = frames ? (uint32_t)((uint64_t)us * rate / frames / 1000) : 0;
    uint32_t cf = s_cost_frames;
    out->decode_permille = cf ? (uint32_t)(s_cost_us * rate / cf / 1000) : 0;
    out->chunk_us_max = s_dec_us_max;
    out->decoder_prio = s_player_task ? s_dec_prio : 0;
    out->stack_free_dec = s_player_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_player_task) : 0;
    out->stack_free_out = s_aout_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_aout_task) : 0;
    return true;
}

bool aos_hal_play_file(const char *path)
{
    return aos_hal_player_play(path);
}

void aos_hal_audio_stop(void)
{
    aos_hal_player_stop();
}

bool aos_hal_audio_is_playing(void)
{
    return s_player_state == AOS_PLAYER_PLAYING;
}

/* The streaming speaker or the microphone asks for the room: pause the music
 * until they are done. Counted, because the walkie-talkie holds one and then
 * the other; and the writer keeps quiet for a moment after the last one
 * lets go, or the music would blip in the gap between releasing the
 * microphone and opening the speaker. On the watch yield=true also waited
 * for the writer to let go of the codec; here there is nothing to let go
 * of, and the music is out of the mix from the next block (10 ms). */
#define PLAYER_YIELD_TAIL_US    (800 * 1000)

static void player_yield_to_app(bool yield)
{
    if (yield) {
        portENTER_CRITICAL(&s_yield_mux);
        bool first = s_yield_refs++ == 0;
        portEXIT_CRITICAL(&s_yield_mux);
        if (!first) {
            return;
        }
        s_player_yielded_pause = s_player_task && s_player_state == AOS_PLAYER_PLAYING;
        if (s_player_yielded_pause) {
            s_player_state = AOS_PLAYER_PAUSED;
        }
    } else {
        portENTER_CRITICAL(&s_yield_mux);
        bool last = s_yield_refs > 0 && --s_yield_refs == 0;
        if (last) {
            s_yield_until_us = esp_timer_get_time() + PLAYER_YIELD_TAIL_US;
        }
        portEXIT_CRITICAL(&s_yield_mux);
        if (last && s_player_yielded_pause && s_player_state == AOS_PLAYER_PAUSED) {
            s_player_state = AOS_PLAYER_PLAYING;
        }
        if (last) {
            s_player_yielded_pause = false;
        }
        aout_kick();
    }
}

/* -------------------------------------------------------------------------- */
/* Streaming speaker                                                           */
/* -------------------------------------------------------------------------- */

/* PCM in, sound out: the walkie-talkie's frames off the air. write() drops
 * samples into a ring of one second in PSRAM and never blocks; the output
 * task pulls them through a resampler (the app's rate -> 48 kHz) into its
 * mix, 10 ms at a time, and plays silence when the ring is empty, so the
 * amplifier stays warm between frames. With nothing waiting the resampler's
 * phase stays put: the app is late, not done.
 *
 * The ring and the resampler change hands under s_spk_lock: open and close
 * come from the UI, the pull from the output task. */
#define SPK_DEFAULT_RATE    16000

static SemaphoreHandle_t s_spk_lock;
static volatile bool     s_spk_open;            /* the app's stream is in the mix */
static bool              s_spk_mixed;           /* ...over the music, which did not yield */
static int16_t          *s_spk_ring;            /* one second at the app's rate, PSRAM */
static uint32_t          s_spk_ring_n;
static volatile uint32_t s_spk_head, s_spk_tail;
static uint32_t          s_spk_rate;
static rs_t              s_spk_rs;              /* the app's rate -> BUS_RATE */

/* One block of the app's stream into 'out' (the rest silence); how many
 * samples of it there were. */
static int spk_block(int16_t *out, int n)
{
    int i = 0;
    if (s_spk_open && s_spk_lock && xSemaphoreTake(s_spk_lock, 0) == pdTRUE) {
        if (s_spk_open && s_spk_ring) {
            rs_t *r = &s_spk_rs;
            uint32_t head = s_spk_head, tail = s_spk_tail;
            RING_FENCE();
            for (; i < n; i++) {
                if (!r->taps) {
                    if (tail == head) {
                        break;
                    }
                    out[i] = s_spk_ring[tail % s_spk_ring_n];
                    tail++;
                    continue;
                }
                bool dry = false;
                while (r->pos >= RS_ONE) {
                    if (tail == head) {
                        dry = true;
                        break;
                    }
                    rs_push(r, s_spk_ring[tail % s_spk_ring_n]);
                    tail++;
                    r->pos -= RS_ONE;
                }
                if (dry) {
                    break;
                }
                out[i] = rs_eval(r);
                r->pos += r->step;
            }
            s_spk_tail = tail;
        }
        xSemaphoreGive(s_spk_lock);
    }
    if (i < n) {
        memset(out + i, 0, (size_t)(n - i) * sizeof(int16_t));
    }
    return i;
}

/* The ring, one second at the app's rate, emptied, and its resampler. */
static bool spk_prepare(uint32_t rate)
{
    if (!s_spk_ring || s_spk_ring_n != rate) {
        int16_t *ring = big_alloc(rate * sizeof(int16_t));
        if (!ring) {
            return false;
        }
        free(s_spk_ring);
        s_spk_ring = ring;
        s_spk_ring_n = rate;
    }
    if (!rs_config(&s_spk_rs, rate, BUS_RATE)) {
        return false;
    }
    rs_reset(&s_spk_rs);
    s_spk_rate = rate;
    s_spk_head = s_spk_tail = 0;
    return true;
}

bool aos_hal_spk_open(uint32_t sample_rate)
{
    if (s_spk_open) {
        return true;
    }
    if (s_hw_state != 1 || !s_speaker || !s_spk_lock) {
        return false;
    }
    uint32_t rate = sample_rate ? sample_rate : SPK_DEFAULT_RATE;
    if (rate < 4000 || rate > BUS_RATE) {
        return false;
    }
    /* Mixing on and music playing: the app's sound goes over the music
     * instead of pausing it. Never the walkie, never with the microphone
     * open (it would be recorded). */
    bool mixed = aos_hal_player_mix() && !s_fg_voice && s_music_on &&
                 s_player_state == AOS_PLAYER_PLAYING && s_mic_users == 0;
    if (!mixed) {
        player_yield_to_app(true);      /* the app in front wins; the music comes back on close */
    }
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    bool ok = spk_prepare(rate);
    if (ok) {
        s_spk_mixed = mixed;
        s_spk_open = true;
    }
    xSemaphoreGive(s_spk_lock);
    if (!ok) {
        ESP_LOGE(TAG, "streaming speaker: no PSRAM for %lu Hz", (unsigned long)rate);
        if (!mixed) {
            player_yield_to_app(false); /* no speaker after all: the music goes on */
        }
        return false;
    }
    aout_kick();
    return true;
}

int aos_hal_spk_write(const int16_t *pcm, int n)
{
    if (!s_spk_open || !pcm || n <= 0 || !s_spk_ring) {
        return 0;
    }
    uint32_t head = s_spk_head;
    uint32_t used = head - s_spk_tail;
    uint32_t room = s_spk_ring_n - used;
    if ((uint32_t)n > room) {
        n = (int)room;
    }
    for (int i = 0; i < n; i++) {
        s_spk_ring[(head + (uint32_t)i) % s_spk_ring_n] = pcm[i];
    }
    RING_FENCE();
    s_spk_head = head + (uint32_t)n;
    return n;
}

int aos_hal_spk_queued(void)
{
    return s_spk_open ? (int)(s_spk_head - s_spk_tail) : 0;
}

bool aos_hal_spk_is_open(void)
{
    return s_spk_open;
}

void aos_hal_spk_close(void)
{
    if (!s_spk_open || !s_spk_lock) {
        return;
    }
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    bool mixed = s_spk_mixed;
    s_spk_open = false;                 /* the mix stops reading it; what was queued is dropped */
    s_spk_mixed = false;
    xSemaphoreGive(s_spk_lock);
    if (!mixed) {
        player_yield_to_app(false);     /* the music comes back */
    }
}

/* -------------------------------------------------------------------------- */
/* The output task                                                             */
/* -------------------------------------------------------------------------- */

static bool out_open(int16_t *stereo)
{
    esp_codec_dev_sample_info_t fs;
    bus_fs(&fs);
    int ret = esp_codec_dev_open(s_speaker, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker: the ES8311 did not open (%d)", ret);
        return false;
    }
    esp_codec_dev_set_out_vol(s_speaker, s_volume);
    /* A breath of silence so the amplifier settles before the first sound;
     * without it, on the watch, the first note after a while was lost. */
    memset(stereo, 0, (size_t)BUS_BLOCK * 2 * sizeof(int16_t));
    for (int ms = 0; ms < OUT_WARMUP_MS; ms += BUS_BLOCK_MS) {
        esp_codec_dev_write(s_speaker, stereo, BUS_BLOCK * 2 * (int)sizeof(int16_t));
    }
    s_out_open = true;
    s_out_opens++;
    return true;
}

/* a USB sound card on the host (aos_usb_uac_p4.c): the mix goes to it and
 * the board's speaker is muted while one is there and wanted */
bool aos_p4_usb_audio_out(void);
void aos_p4_usb_audio_write(const int16_t *stereo, int frames, int volume);
void aos_p4_usb_audio_idle(void);

static void out_close(void)
{
    aos_p4_usb_audio_idle();
    esp_codec_dev_close(s_speaker);
    s_out_open = false;
    ESP_LOGD(TAG, "speaker closed (amp cycle %lu)", (unsigned long)s_out_opens);
}

/* The codecs, through the BSP: one I2S pair at the bus format, the ES8311 on
 * it for output and the ES7210 for input. */
static void audio_hw_init(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BUS_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    esp_err_t e = bsp_audio_init(&cfg);
    if (e == ESP_OK) {
        s_speaker = bsp_audio_codec_speaker_init();
        s_mic     = bsp_audio_codec_microphone_init();
    }
    if (s_speaker) {
        /* esp_codec_dev's own curve is a straight line in dB from -50 at 0
         * to 0 at 100: half way was -25 dB, and on the board the bottom half
         * of the slider was barely heard (2026-09-29). -10 dB sounds about
         * half as loud, so that is where the middle goes; below 1 is mute. */
        static esp_codec_dev_vol_map_t points[] = {
            { .vol = 1,   .db_value = -36.0f },
            { .vol = 25,  .db_value = -20.0f },
            { .vol = 50,  .db_value = -10.0f },
            { .vol = 75,  .db_value = -4.0f },
            { .vol = 100, .db_value = 0.0f },
        };
        esp_codec_dev_vol_curve_t curve = { .vol_map = points, .count = sizeof points / sizeof points[0] };
        esp_codec_dev_set_vol_curve(s_speaker, &curve);
    }
    ESP_LOGI(TAG, "I2S %s, ES8311 %s, ES7210 %s; bus %d Hz 16-bit stereo, volume %d",
             esp_err_to_name(e), s_speaker ? "ok" : "MISSING", s_mic ? "ok" : "MISSING",
             BUS_RATE, s_volume);
    s_hw_state = (s_speaker || s_mic) ? 1 : -1;
}

static void aout_task(void *arg)
{
    (void)arg;
    audio_hw_init();
    if (!s_speaker) {
        /* No speaker: nothing for this task to do. The microphone does not
         * need it (TX, its clock, was enabled by bsp_audio_init and the
         * ES7210's open reconfigures it). */
        s_aout_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* The mixer's blocks: four of 10 ms, 3.8 KB, in PSRAM (the I2S driver
     * copies the stereo one into its own DMA buffers). */
    int16_t *mus    = big_alloc((size_t)BUS_BLOCK * sizeof(int16_t));
    int16_t *app    = big_alloc((size_t)BUS_BLOCK * sizeof(int16_t));
    int16_t *ton    = big_alloc((size_t)BUS_BLOCK * sizeof(int16_t));
    int16_t *stereo = big_alloc((size_t)BUS_BLOCK * 2 * sizeof(int16_t));
    if (!mus || !app || !ton || !stereo) {
        ESP_LOGE(TAG, "speaker: no PSRAM for the mixer");
        s_aout_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int open_vol = -1;
    uint32_t idle_ms = 0;
    int usb_muted = -1;             /* the speaker muted for a USB card; -1 not known (just opened) */

    for (;;) {
        bool for_mic = s_out_for_mic;
        if (!s_out_open) {
            s_out_mic_ack = false;
            music_idle();
            bool need = s_spk_open || for_mic || tone_pending() || music_ready();
            if (!need) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
                continue;
            }
            if (!out_open(stereo)) {
                /* nothing will make a sound: let the player go, drop the notes */
                if (s_music_on) {
                    s_player_abort = true;
                    music_end();
                }
                if (s_tone_queue) {
                    xQueueReset(s_tone_queue);
                }
                s_tone_total = s_tone_done = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            open_vol = s_volume;
            idle_ms = 0;
            usb_muted = -1;
        }
        if (open_vol != s_volume) {         /* the slider is heard at once */
            open_vol = s_volume;
            esp_codec_dev_set_out_vol(s_speaker, open_vol);
        }

        int nm = music_block(mus);
        int na = spk_block(app, BUS_BLOCK);
        int nt = tone_block(ton, BUS_BLOCK);
        int32_t gain = (nm && s_spk_open) ? MIX_MUSIC_GAIN_Q8 : 256;
        for (int i = 0; i < BUS_BLOCK; i++) {
            int16_t v = sat16(((mus[i] * gain) >> 8) + app[i] + ton[i]);
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }
        /* A USB sound card: the same block to it, and the speaker silent.
         * The codec keeps running: its clock paces this loop and the
         * board's microphones. */
        bool usb = aos_p4_usb_audio_out();
        if ((int)usb != usb_muted) {
            esp_codec_dev_set_out_mute(s_speaker, usb);
            usb_muted = usb;
        }
        if (usb) aos_p4_usb_audio_write(stereo, BUS_BLOCK, s_volume);
        if (esp_codec_dev_write(s_speaker, stereo, BUS_BLOCK * 2 * (int)sizeof(int16_t)) !=
            ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker: write failed, reopening");
            out_close();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 'for_mic' was read before this block: acknowledging it here means
         * this very iteration will not count towards the idle close. */
        s_out_mic_ack = for_mic;
        bool busy = nm || na || nt || s_spk_open || for_mic || tone_pending();
        idle_ms = busy ? 0 : idle_ms + BUS_BLOCK_MS;
        if (idle_ms >= OUT_IDLE_MS) {
            /* Nothing for five seconds and no capture (closing under one is
             * harmless, reopening under one is not: the header). */
            out_close();
            idle_ms = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Recorder                                                                    */
/* -------------------------------------------------------------------------- */

/* The WAV header the recorder writes (reading WAV lives in aos_audio.c). */
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t size;
    char     wave[4];
} wav_riff_t;

typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} wav_chunk_t;

typedef struct __attribute__((packed)) {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
} wav_fmt_t;

/* A task reads blocks from the microphone and writes them to the WAV as they
 * are: 16-bit mono PCM, uncompressed. At 16 kHz that is 32 KB per second,
 * ~2 MB per minute, which is nothing for a microSD and saves having to bring
 * in an encoder.
 *
 * The header is written twice: on open with the sizes at zero, and on close
 * with the real ones. If the power goes in between, what is left is a WAV
 * claiming 0 bytes of audio, so every two seconds the header is rewritten
 * with the figures so far: ending a recording badly costs at most the last
 * two seconds. */

#define REC_RING_LEN    256     /* ~12 s of envelope at 20 Hz */
/* ES7210 PGA. The watch's 30 dB was measured for its ES8311 and a microphone
 * a hand's width from the mouth; this board is a 5" panel read from half a
 * metre and a different ADC. 30 is the driver's own default and the first
 * guess: day one, measurement 3. */
#define MIC_GAIN_DB     30
#define MIC_GAIN_MAX    36
/* The first few milliseconds are the finger hitting the glass: an impulse
 * that stays in the file as a click. Three blocks (150 ms) are discarded;
 * nobody starts talking that soon after pressing. */
#define REC_SKIP_BLOCKS 3

/* One capture, two consumers: the recorder and the raw microphone are two
 * clients of the same task. The task lives for as long as some user remains
 * and shuts itself down on seeing s_mic_users at zero. Users come and go
 * from the UI thread, so it is enough for the counters to be volatile. */
#define MIC_USER_REC    0x01    /* the recorder */
#define MIC_USER_RAW    0x02    /* aos_hal_mic_open() */

static TaskHandle_t      s_mic_task;
static uint32_t          s_mic_rate = AOS_MIC_RATE_HZ;  /* set by whoever is first */
static volatile bool     s_mic_running;  /* the codec really is capturing */
static volatile int      s_mic_level;    /* 0..100 of the last block */
static volatile int      s_mic_peak;     /* raw peak of the last block */
static int               s_mic_gain_db = MIC_GAIN_DB;
static volatile bool     s_mic_gain_dirty;

/* Ring of raw PCM for the app: one second, in PSRAM, allocated the first time
 * somebody opens the raw microphone and NEVER freed (the task can never find
 * it freed from under a memcpy while the recorder keeps the capture alive). */
static int16_t          *s_pcm_ring;
static uint32_t          s_pcm_len;      /* in samples */
static volatile uint32_t s_pcm_w;        /* monotonic counters, not indices */
static volatile uint32_t s_pcm_r;
static volatile uint32_t s_pcm_dropped;

static aos_rec_state_t   s_rec_state;
static char              s_rec_path[160];
static uint32_t          s_rec_rate = AOS_REC_RATE_HZ;
static volatile uint32_t s_rec_bytes;
static volatile bool     s_rec_abort;

static uint8_t           s_rec_ring[REC_RING_LEN];
static volatile uint32_t s_rec_ring_w;   /* monotonic counters, not indices */
static volatile uint32_t s_rec_ring_r;

static void rec_header_write(FILE *file, uint32_t rate, uint32_t data_bytes)
{
    const uint16_t channels = 1;
    const uint16_t bits     = 16;

    wav_riff_t riff = { .riff = {'R','I','F','F'}, .wave = {'W','A','V','E'} };
    riff.size = 36 + data_bytes;

    wav_chunk_t fmt_chunk  = { .id = {'f','m','t',' '}, .size = 16 };
    wav_fmt_t   fmt = {
        .format      = 1,                       /* PCM */
        .channels    = channels,
        .sample_rate = rate,
        .byte_rate   = rate * channels * bits / 8,
        .block_align = channels * bits / 8,
        .bits        = bits,
    };
    wav_chunk_t data_chunk = { .id = {'d','a','t','a'}, .size = data_bytes };

    fseek(file, 0, SEEK_SET);
    fwrite(&riff, sizeof(riff), 1, file);
    fwrite(&fmt_chunk, sizeof(fmt_chunk), 1, file);
    fwrite(&fmt, sizeof(fmt), 1, file);
    fwrite(&data_chunk, sizeof(data_chunk), 1, file);
}

/* A VU level goes in dB, not in linear: -48 dBFS..0 dBFS is mapped to
 * 0..100, so a normal voice falls in the middle of the scale, where the eye
 * expects to see it, and not at 12 of 100. */
static uint8_t rec_level_from_peak(int32_t peak)
{
    if (peak < 16) {
        return 0;                   /* the ADC's noise floor */
    }
    float db = 20.0f * log10f((float)peak / 32768.0f);
    if (db < -48.0f) {
        return 0;
    }
    int level = (int)((db + 48.0f) * (100.0f / 48.0f) + 0.5f);
    return level > 100 ? 100 : (uint8_t)level;
}

static void rec_push_peak(uint8_t peak)
{
    s_rec_ring[s_rec_ring_w % REC_RING_LEN] = peak;
    RING_FENCE();
    s_rec_ring_w++;
}

/* Pushes the block into the raw PCM ring. Only if somebody is listening:
 * while merely recording, the copy is of use to nobody. */
static void pcm_push(const int16_t *samples, int count)
{
    if (!s_pcm_ring || !s_pcm_len) {
        return;
    }
    uint32_t w = s_pcm_w;
    for (int i = 0; i < count; i++) {
        s_pcm_ring[(w + (uint32_t)i) % s_pcm_len] = samples[i];
    }
    RING_FENCE();
    s_pcm_w = w + (uint32_t)count;
}

/* Statistics of one recording. Reset when each file is opened, not when the
 * capture starts: one capture may see several recordings go by. */
typedef struct {
    int32_t  max_raw;
    uint32_t blocks;
    uint32_t clipped;
    uint32_t level_sum;
    uint32_t since_flush;
    int      warmup;
} rec_stats_t;

/* Closes the WAV: final header, the log line that makes it possible to tune
 * the gain with data, and deleting the file if nothing made it in. */
static void rec_finish(FILE **file, rec_stats_t *st)
{
    if (!*file) {
        return;
    }
    rec_header_write(*file, s_rec_rate, s_rec_bytes);
    fclose(*file);
    *file = NULL;

    /* The peak alone is not enough: a sharp knock also takes it to the
     * ceiling. The proportion of saturated blocks tells "a loud noise" apart
     * from "the gain is wrong", and the mean level says how much headroom went
     * unused. */
    ESP_LOGI(TAG, "recording finished: %u ms, peak %ld of 32768 (%d%%), "
                  "%u%% of %u blocks clipped, average level %u/100, gain %d dB",
             (unsigned)(s_rec_rate ? (uint32_t)((uint64_t)s_rec_bytes * 500 / s_rec_rate) : 0),
             (long)st->max_raw, (int)(st->max_raw * 100 / 32768),
             (unsigned)(st->blocks ? st->clipped * 100 / st->blocks : 0),
             (unsigned)st->blocks,
             (unsigned)(st->blocks ? st->level_sum / st->blocks : 0), s_mic_gain_db);

    /* If not even a fifth of a second made it in, the capture failed: delete
     * the file instead of leaving a zero-second WAV on the card. */
    if (s_rec_bytes < s_rec_rate / 5 * 2) {
        ESP_LOGW(TAG, "empty recording, deleting %s", s_rec_path);
        remove(s_rec_path);
        s_rec_bytes = 0;
    }
    s_rec_state = AOS_REC_IDLE;
}

/* ---- the capture: the ES7210 into a ring -------------------------------------
 *
 * aos_ain owns the ES7210 while a capture runs: 10 ms of stereo 48 kHz at a
 * time, one microphone of the two, brought to the capture's rate, into two
 * seconds of ring. The mic task (the watch's, below) drains it. */

#define CAP_RING_S      2
#define CAP_DECIDE_MS   500         /* when the dead-microphone check is made */
#define CAP_DEAD_RATIO  16          /* 24 dB: one slot this far under the other is dead */

static TaskHandle_t      s_ain_task;
static volatile bool     s_ain_stop;
static volatile bool     s_ain_open;
static volatile bool     s_ain_failed;
static int16_t          *s_cap_ring;            /* CAP_RING_S at up to 48 kHz, PSRAM, never freed */
static uint32_t          s_cap_n;               /* samples, for the rate in force */
static volatile uint32_t s_cap_w, s_cap_r;      /* monotonic */
static uint32_t          s_cap_lost;
static rs_t              s_cap_rs;              /* BUS_RATE -> s_mic_rate */

static void ain_task(void *arg)
{
    (void)arg;
    esp_codec_dev_sample_info_t fs;
    bus_fs(&fs);
    int ret = esp_codec_dev_open(s_mic, &fs);
    int16_t *raw = NULL, *mono = NULL, *conv = NULL;
    if (ret == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_in_gain(s_mic, (float)s_mic_gain_db);
        s_mic_gain_dirty = false;
        raw  = big_alloc((size_t)BUS_BLOCK * 2 * sizeof(int16_t));
        mono = big_alloc((size_t)BUS_BLOCK * sizeof(int16_t));
    }
    bool rs_ok = ret == ESP_CODEC_DEV_OK && rs_config(&s_cap_rs, BUS_RATE, s_mic_rate);
    if (rs_ok) {
        rs_reset(&s_cap_rs);
        conv = big_alloc((size_t)rs_max_out(&s_cap_rs, BUS_BLOCK) * sizeof(int16_t));
    }
    if (ret != ESP_CODEC_DEV_OK || !raw || !mono || !conv) {
        ESP_LOGE(TAG, "microphone: the ES7210 did not open (%d) or no PSRAM", ret);
        if (ret == ESP_CODEC_DEV_OK) {
            esp_codec_dev_close(s_mic);
        }
        free(raw);
        free(mono);
        free(conv);
        s_ain_failed = true;
        s_ain_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_ain_open = true;

    /* Per slot: the peak and the mean of |x| since the start, for the
     * dead-microphone check and the log that tells which slot is which. */
    uint64_t sum[2] = { 0, 0 };
    int32_t  pk[2] = { 0, 0 };
    uint32_t frames = 0;
    int      slot = 0;              /* MIC1 */
    bool     decided = false;
    int      errors = 0;

    while (!s_ain_stop) {
        if (s_mic_gain_dirty) {
            s_mic_gain_dirty = false;
            esp_codec_dev_set_in_gain(s_mic, (float)s_mic_gain_db);
        }
        ret = esp_codec_dev_read(s_mic, raw, BUS_BLOCK * 2 * (int)sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            /* A single error must not cost the whole capture. */
            ESP_LOGE(TAG, "microphone read: %d (failure %d)", ret, errors + 1);
            if (++errors >= 8) {
                s_ain_failed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        errors = 0;

        for (int i = 0; i < BUS_BLOCK; i++) {
            int32_t l = raw[2 * i], r = raw[2 * i + 1];
            l = l < 0 ? -l : l;
            r = r < 0 ? -r : r;
            sum[0] += (uint64_t)l;
            sum[1] += (uint64_t)r;
            if (l > pk[0]) pk[0] = l;
            if (r > pk[1]) pk[1] = r;
            mono[i] = raw[2 * i + slot];
        }
        frames += BUS_BLOCK;
        if (!decided && frames >= (uint32_t)BUS_RATE * CAP_DECIDE_MS / 1000) {
            decided = true;
            /* MIC1 unless it looks dead and MIC2 does not: a slot with a
             * microphone has at least its noise floor, a dead one sits at a
             * few LSB or at zero. */
            if (sum[1] > 8ull * frames && sum[0] * CAP_DEAD_RATIO < sum[1]) {
                slot = 1;
                ESP_LOGW(TAG, "microphone: MIC1 looks dead (mean %u, MIC2 %u): using MIC2",
                         (unsigned)(sum[0] / frames), (unsigned)(sum[1] / frames));
            }
        }

        int m = rs_process(&s_cap_rs, mono, BUS_BLOCK, conv);
        uint32_t w = s_cap_w;
        for (int i = 0; i < m; i++) {
            s_cap_ring[(w + (uint32_t)i) % s_cap_n] = conv[i];
        }
        RING_FENCE();
        s_cap_w = w + (uint32_t)m;
        TaskHandle_t t = s_mic_task;
        if (t) {
            xTaskNotifyGive(t);
        }
    }

    esp_codec_dev_close(s_mic);
    if (frames) {
        ESP_LOGI(TAG, "capture: %lu ms, MIC1 mean %u peak %ld, MIC2 mean %u peak %ld, used MIC%d, "
                      "%lu samples lost", (unsigned long)((uint64_t)frames * 1000 / BUS_RATE),
                 (unsigned)(sum[0] / frames), (long)pk[0], (unsigned)(sum[1] / frames),
                 (long)pk[1], slot + 1, (unsigned long)s_cap_lost);
    }
    free(raw);
    free(mono);
    free(conv);
    s_ain_open = false;
    s_ain_task = NULL;
    vTaskDelete(NULL);
}

/* 'n' samples of the capture into 'out', waiting for them up to 'wait_ms'.
 * False if they did not come (the capture failed or stalled). */
static bool cap_read(int16_t *out, int n, uint32_t wait_ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    for (;;) {
        uint32_t w = s_cap_w;
        RING_FENCE();
        uint32_t pending = w - s_cap_r;
        if (pending > s_cap_n) {        /* we fell asleep: the oldest are gone */
            s_cap_lost += pending - s_cap_n;
            s_cap_r = w - s_cap_n;
            pending = s_cap_n;
        }
        if (pending >= (uint32_t)n) {
            uint32_t r = s_cap_r;
            for (int i = 0; i < n; i++) {
                out[i] = s_cap_ring[(r + (uint32_t)i) % s_cap_n];
            }
            s_cap_r = r + (uint32_t)n;
            return true;
        }
        if (s_ain_failed || !s_ain_task || esp_timer_get_time() > until) {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

/* Asks the output task for the bus clock and starts the ES7210 on it. */
static bool cap_start(void)
{
    /* The speaker first: opening it under a running capture would stop the
     * capture's clock (the header). Bounded: without a speaker the mic still
     * works, TX runs from bsp_audio_init. */
    s_out_for_mic = true;
    aout_kick();
    for (int i = 0; i < 50 && s_speaker && s_aout_task && !s_out_mic_ack; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_cap_ring) {
        s_cap_ring = big_alloc((size_t)BUS_RATE * CAP_RING_S * sizeof(int16_t));
        if (!s_cap_ring) {
            ESP_LOGE(TAG, "microphone: no PSRAM for the capture ring");
            return false;
        }
    }
    s_cap_n = s_mic_rate * CAP_RING_S;
    s_cap_w = s_cap_r = 0;
    s_cap_lost = 0;
    s_ain_stop = false;
    s_ain_open = false;
    s_ain_failed = false;
    /* 4 KB internal: the codec's open and its logs. The handle is written
     * before the task can run, so it never outlives a quick failure. */
    if (xTaskCreatePinnedToCore(ain_task, "aos_ain", 4096, NULL, IN_PRIO, &s_ain_task,
                                AUDIO_CORE) != pdPASS) {
        s_ain_task = NULL;
        return false;
    }
    for (int i = 0; i < 100 && !s_ain_open && !s_ain_failed && s_ain_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_ain_open;
}

static void cap_stop(void)
{
    s_ain_stop = true;
    for (int i = 0; i < 100 && s_ain_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_out_for_mic = false;
    aout_kick();
}

static void mic_task(void *arg)
{
    (void)arg;

    FILE       *file = NULL;
    rec_stats_t st   = { 0 };

    if (!cap_start()) {
        ESP_LOGE(TAG, "the microphone did not start at %lu Hz", (unsigned long)s_mic_rate);
        cap_stop();
        s_rec_state = AOS_REC_IDLE;
        s_mic_users = 0;
        s_mic_task  = NULL;
        player_yield_to_app(false);
        vTaskDelete(NULL);
        return;
    }
    s_mic_running = true;

    /* One block = one envelope sample, so the peak comes for free. */
    const int block = (int)(s_mic_rate / AOS_REC_PEAK_HZ);
    int16_t *buffer = heap_caps_malloc((size_t)block * sizeof(int16_t),
                                       MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buffer) {
        buffer = big_alloc((size_t)block * sizeof(int16_t));
    }
    int errors = 0;

    while (buffer && s_mic_users) {
        /* Recording starts and stops: the file is opened and closed by the
         * task, which is the only one that writes it. aos_hal_rec_start() only
         * declares the intent. */
        if (!file && (s_mic_users & MIC_USER_REC) &&
            !s_rec_abort && s_rec_state != AOS_REC_IDLE) {
            file = fopen(s_rec_path, "wb");
            if (!file) {
                ESP_LOGE(TAG, "could not create %s", s_rec_path);
                s_rec_state  = AOS_REC_IDLE;
                s_mic_users &= ~(uint32_t)MIC_USER_REC;
                continue;
            }
            rec_header_write(file, s_rec_rate, 0);
            st = (rec_stats_t){ .warmup = REC_SKIP_BLOCKS };
        }
        if (file && (s_rec_abort || !(s_mic_users & MIC_USER_REC))) {
            rec_finish(&file, &st);
            s_mic_users &= ~(uint32_t)MIC_USER_REC;
            continue;
        }

        if (!cap_read(buffer, block, 500)) {
            /* A stall is retried a few times before the capture is given up. */
            ESP_LOGE(TAG, "microphone: no audio (failure %d)", errors + 1);
            if (s_ain_failed || !s_ain_task || ++errors >= 8) {
                break;
            }
            continue;
        }
        errors = 0;

        int32_t peak = 0;
        for (int i = 0; i < block; i++) {
            int32_t value = buffer[i] < 0 ? -buffer[i] : buffer[i];
            if (value > peak) {
                peak = value;
            }
        }
        s_mic_peak  = (int)peak;
        s_mic_level = rec_level_from_peak(peak);
        rec_push_peak((uint8_t)s_mic_level);

        if (s_mic_users & MIC_USER_RAW) {
            pcm_push(buffer, block);
        }

        if (!file || s_rec_state != AOS_REC_RECORDING) {
            continue;       /* not recording or paused: the capture stays alive */
        }
        if (st.warmup > 0) {
            st.warmup--;            /* the finger's knock does not make it into the file */
            continue;
        }

        if (peak > st.max_raw) {
            st.max_raw = peak;
        }
        st.blocks++;
        if (peak >= 32000) {
            st.clipped++;      /* pinned to the ceiling: that is clipping */
        }
        st.level_sum += (uint32_t)s_mic_level;

        if (fwrite(buffer, 1, (size_t)block * sizeof(int16_t), file) == 0) {
            ESP_LOGE(TAG, "no more audio fits on the card");
            s_rec_abort = true;
            continue;
        }
        s_rec_bytes += (uint32_t)block * sizeof(int16_t);

        st.since_flush += (uint32_t)block * sizeof(int16_t);
        if (st.since_flush >= s_rec_rate * 2 * 2) {     /* every ~2 s */
            st.since_flush = 0;
            rec_header_write(file, s_rec_rate, s_rec_bytes);
            fseek(file, 0, SEEK_END);
            fflush(file);
        }
    }

    rec_finish(&file, &st);
    free(buffer);
    cap_stop();

    s_mic_running     = false;
    s_mic_level       = 0;
    s_mic_peak        = 0;
    s_rec_state       = AOS_REC_IDLE;
    s_mic_users       = 0;
    s_mic_task        = NULL;
    player_yield_to_app(false);     /* and the music comes back */
    vTaskDelete(NULL);
}

/* Adds one user to the capture and starts it if it was needed. */
static bool mic_acquire(uint32_t user)
{
    if (s_hw_state != 1 || !s_mic) {
        return false;
    }
    s_mic_users |= user;
    if (s_mic_task) {
        return true;
    }
    player_yield_to_app(true);          /* the music pauses while we listen */
    /* 4 KB internal: FAT (the WAV). Pinned: rec_level_from_peak() is float. */
    if (xTaskCreatePinnedToCore(mic_task, "aos_mic", 4096, NULL, MIC_PRIO, &s_mic_task,
                                AUDIO_CORE) != pdPASS) {
        s_mic_task = NULL;
        s_mic_users &= ~user;
        player_yield_to_app(false);
        return false;
    }
    return true;
}

/* Removes a user. The task shuts down on its own once none are left. */
static void mic_release(uint32_t user)
{
    s_mic_users &= ~user;
}

bool aos_hal_rec_start(const char *path, uint32_t sample_rate)
{
    if (!path || s_rec_state != AOS_REC_IDLE || (s_mic_users & MIC_USER_REC)) {
        return false;
    }

    snprintf(s_rec_path, sizeof(s_rec_path), "%s", path);
    /* If a capture is already running (the tuner, say), the rate is the one
     * already in force: changing it would mean closing it under the other. */
    if (!s_mic_task) {
        s_mic_rate = sample_rate ? sample_rate : AOS_REC_RATE_HZ;
        if (s_mic_rate < 4000 || s_mic_rate > BUS_RATE) {
            s_mic_rate = AOS_REC_RATE_HZ;
        }
    }
    s_rec_rate     = s_mic_rate;
    s_rec_bytes    = 0;
    s_rec_abort    = false;
    s_rec_ring_w   = 0;
    s_rec_ring_r   = 0;
    s_rec_state    = AOS_REC_RECORDING;

    if (!mic_acquire(MIC_USER_REC)) {
        s_rec_state = AOS_REC_IDLE;
        return false;
    }
    return true;
}

void aos_hal_rec_pause(void)
{
    if (s_rec_state == AOS_REC_RECORDING) {
        s_rec_state = AOS_REC_PAUSED;
    }
}

void aos_hal_rec_resume(void)
{
    if (s_rec_state == AOS_REC_PAUSED) {
        s_rec_state = AOS_REC_RECORDING;
    }
}

bool aos_hal_rec_stop(void)
{
    if (!(s_mic_users & MIC_USER_REC)) {
        s_rec_state = AOS_REC_IDLE;
        return s_rec_bytes > 0;
    }

    /* We wait for the task to close the file, not to die: with the raw
     * microphone open the capture stays alive after the recording. */
    s_rec_abort = true;
    for (int i = 0; i < 100 && (s_mic_users & MIC_USER_REC); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    mic_release(MIC_USER_REC);
    s_rec_state = AOS_REC_IDLE;
    return s_rec_bytes > 0;
}

bool aos_hal_rec_status(aos_rec_status_t *out)
{
    if (!out) {
        return false;
    }
    out->state       = s_rec_state;
    out->bytes       = s_rec_bytes;
    out->sample_rate = s_rec_rate;
    out->channels    = 1;
    out->level       = s_mic_level;
    /* The time comes from the bytes written, not from the clock: that way what
     * the screen says is exactly how long the file will be. */
    out->elapsed_ms  = s_rec_rate
                     ? (uint32_t)((uint64_t)s_rec_bytes * 500 / s_rec_rate)
                     : 0;
    snprintf(out->path, sizeof(out->path), "%s", s_rec_path);
    return true;
}

int aos_hal_rec_peaks(uint8_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }

    uint32_t write = s_rec_ring_w;
    RING_FENCE();
    uint32_t pending = write - s_rec_ring_r;
    if (pending > REC_RING_LEN) {       /* the app fell asleep: we lost the old data */
        s_rec_ring_r = write - REC_RING_LEN;
        pending = REC_RING_LEN;
    }
    if (pending > (uint32_t)max) {
        s_rec_ring_r = write - (uint32_t)max;
        pending = (uint32_t)max;
    }

    for (uint32_t i = 0; i < pending; i++) {
        out[i] = s_rec_ring[(s_rec_ring_r + i) % REC_RING_LEN];
    }
    s_rec_ring_r += pending;
    return (int)pending;
}

/* -------------------------------------------------------------------------- */
/* Raw microphone                                                              */
/* -------------------------------------------------------------------------- */

bool aos_hal_mic_open(uint32_t sample_rate)
{
    if (s_mic_users & MIC_USER_RAW) {
        return true;                /* it was already open */
    }
    if (s_hw_state != 1 || !s_mic) {
        return false;
    }

    /* The real rate: if there is already a capture, whichever one is running.
     * Any rate from 4 to 48 kHz is delivered exactly (the resampler), so the
     * tuner's 16 and the meter's 32 kHz both come out as asked. */
    uint32_t rate = s_mic_task ? s_mic_rate
                               : (sample_rate ? sample_rate : AOS_MIC_RATE_HZ);
    if (rate < 4000 || rate > BUS_RATE) {
        rate = AOS_MIC_RATE_HZ;
    }

    /* The ring only grows while the capture is stopped: nobody is writing it
     * then. If it turned out small with the capture running it is left as it
     * is — that is less than a second of history, which is not an error. */
    if (!s_pcm_ring || (s_pcm_len < rate && !s_mic_task)) {
        int16_t *ring = heap_caps_malloc((size_t)rate * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring) {
            ESP_LOGE(TAG, "no PSRAM for the microphone ring");
            return false;
        }
        free(s_pcm_ring);
        s_pcm_ring = ring;
        s_pcm_len  = rate;
    }

    s_pcm_w       = 0;
    s_pcm_r       = 0;
    s_pcm_dropped = 0;

    if (!s_mic_task) {
        s_mic_rate = rate;
    }
    return mic_acquire(MIC_USER_RAW);
}

void aos_hal_mic_close(void)
{
    mic_release(MIC_USER_RAW);
}

int aos_hal_mic_read(int16_t *out, int max)
{
    if (!out || max <= 0 || !s_pcm_ring || !s_pcm_len) {
        return 0;
    }

    uint32_t write   = s_pcm_w;
    RING_FENCE();
    uint32_t pending = write - s_pcm_r;
    if (pending > s_pcm_len) {          /* the app fell asleep: we lost the old data */
        s_pcm_dropped += pending - s_pcm_len;
        s_pcm_r = write - s_pcm_len;
        pending = s_pcm_len;
    }
    if (pending > (uint32_t)max) {
        pending = (uint32_t)max;
    }

    for (uint32_t i = 0; i < pending; i++) {
        out[i] = s_pcm_ring[(s_pcm_r + i) % s_pcm_len];
    }
    s_pcm_r += pending;
    return (int)pending;
}

int aos_hal_mic_available(void)
{
    if (!s_pcm_ring || !s_pcm_len) {
        return 0;
    }
    uint32_t pending = s_pcm_w - s_pcm_r;
    return (int)(pending > s_pcm_len ? s_pcm_len : pending);
}

bool aos_hal_mic_status(aos_mic_status_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->open        = s_mic_running;
    out->sample_rate = s_mic_rate;
    out->gain_db     = s_mic_gain_db;
    out->level       = s_mic_level;
    out->peak        = s_mic_peak;
    out->dropped     = s_pcm_dropped;
    return true;
}

/* The ES7210's PGA in 3 dB steps, 0..36. 33 does not exist: esp_codec_dev
 * 1.5 (es7210.c, get_db) sends 33 dB to its 30 dB code, so it is rounded
 * down here and the status tells the truth. Not kept across restarts (see
 * aos_hal.h). */
void aos_hal_mic_gain_set(int db)
{
    if (db < 0) {
        db = 0;
    } else if (db > MIC_GAIN_MAX) {
        db = MIC_GAIN_MAX;
    }
    db = (db + 1) / 3 * 3;
    if (db == 33) {
        db = 30;
    }
    if (db == s_mic_gain_db) {
        return;
    }
    s_mic_gain_db    = db;
    s_mic_gain_dirty = true;        /* applied by the capture task, owner of the codec */
}

int aos_hal_mic_gain_get(void)
{
    return s_mic_gain_db;
}

int aos_hal_mic_level(void)
{
    /* Valid whenever there is a capture, whether from the recorder or from
     * aos_hal_mic_open(). */
    return s_mic_level;
}

/* -------------------------------------------------------------------------- */
/* Volume and start                                                            */
/* -------------------------------------------------------------------------- */

int aos_hal_volume_get(void)
{
    return s_volume;
}

void aos_hal_volume_set(int percent)
{
    s_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    aos_hal_pref_set_i32("volume", s_volume);   /* the output task applies it on its next block */
}

bool aos_audio_p4_up(void)
{
    return s_hw_state == 1 && s_speaker && s_mic;
}

void aos_audio_p4_start(void)
{
    if (s_tone_queue) {
        return;
    }
    int32_t saved = 0;
    if (aos_hal_pref_get_i32("volume", &saved)) {
        s_volume = saved < 0 ? 0 : (saved > 100 ? 100 : (int)saved);
    }
    /* The recordings folder has to exist before anybody records: fopen() does
     * not create directories. */
    if (aos_hal_sd_present()) {
        mkdir(aos_hal_path_recordings(), 0777);
    }
    s_tone_queue = xQueueCreate(TONE_QUEUE_LEN, sizeof(tone_note_t));
    s_spk_lock   = xSemaphoreCreateMutex();
    /* The touch brought the shared I2C bus up already; this is a no-op then,
     * and it has to happen on this thread (the BSP's flag is not atomic). */
    if (!s_tone_queue || !s_spk_lock || bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio: no queue, lock or I2C");
        s_hw_state = -1;
        return;
    }
    /* 6 KB internal: bringing the codecs up (I2C, the I2S driver and their
     * logs) goes deeper than the mixing. Pinned: the tones are float. */
    if (xTaskCreatePinnedToCore(aout_task, "aos_aout", 6144, NULL, OUT_PRIO, &s_aout_task,
                                AUDIO_CORE) != pdPASS) {
        s_aout_task = NULL;
        s_hw_state = -1;
        ESP_LOGE(TAG, "audio: no memory for the output task");
    }
}
