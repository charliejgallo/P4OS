/*
 * P4OS - USB sound cards on the host (aos_hal.h, docs/USB.md).
 *
 * Espressif's usb_host_uac does the USB Audio Class: it says when a card's
 * output (TX: a speaker, headphones) or input (RX: a microphone) appears,
 * negotiates the stream and moves isochronous packets. This file keeps the
 * card that is there and hands the board's own audio to it.
 *
 * The board mixes everything that sounds (the player, the apps' streaming
 * speaker, the tones) into one 48 kHz 16-bit stereo block every 10 ms
 * (aos_audio_p4.c, its output task). With a card's output there and the
 * setting on (pref usb_audio, on by default), that same block is written
 * to the card, mono-mixed if the card has one channel, and the board's
 * speaker is muted; the codec itself keeps running, as its I2S clock paces
 * the mix and is the board microphones' clock too. The write does not
 * wait: both sides run off the same crystal, so a few milliseconds of
 * buffer in the card's ring are enough, and a block that does not fit is
 * dropped rather than holding the mix.
 *
 * The volume is the board's, on the same curve the speaker has (in dB,
 * -36 at 1 to 0 at 100), applied to the samples; the card's own volume, if
 * it has one, is set to its top once, so the slider sounds the same on
 * both. The stream starts with the first block and stops when the board's
 * audio goes idle (the output task closes the codec after 5 s of nothing).
 *
 * A card that does not take 48 kHz 16-bit PCM is listed but not used.
 */
#include "aos_hal.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "usb";
#define PREF_AUDIO  "usb_audio"
#define RATE        48000
#define RING_BYTES  (RATE / 1000 * 4 * 60)      /* 60 ms of stereo 16-bit */

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

typedef struct {
    uac_host_device_handle_t h;
    uint8_t addr, iface, channels;
    bool ok;                        /* takes 48 kHz 16-bit */
    bool streaming;
    char name[48];
} card_t;

/* what the driver's callbacks hand to this file's task: they run on the
 * driver's own task, where opening a device (which waits for transfers that
 * task processes) would hang */
typedef struct {
    uint8_t kind;                   /* 0 connected, 1 gone */
    uint8_t addr, iface;
    bool tx;
    uac_host_device_handle_t h;
} uac_msg_t;

static struct {
    bool installed;
    volatile bool stop, done;
    QueueHandle_t q;
    SemaphoreHandle_t mx;
    card_t out, in;                 /* the card's output (TX) and input (RX) */
    int vol;                        /* the volume applied, and its gain in Q15 */
    int32_t gain;
    uint32_t written, dropped;
} A;

static void wide_to_str(const wchar_t *w, char *out, size_t n)
{
    size_t i = 0;
    for (; w && w[i] && i + 1 < n; i++) out[i] = w[i] >= 32 && w[i] < 127 ? (char)w[i] : '?';
    out[i] = 0;
    while (i && out[i - 1] == ' ') out[--i] = 0;
}

/* the board's volume curve (aos_audio_p4.c: the speaker's), to a gain */
static int32_t gain_of(int vol)
{
    static const struct { int v; float db; } P[] = { { 1, -36 }, { 25, -20 }, { 50, -10 }, { 75, -4 }, { 100, 0 } };
    if (vol < 1) return 0;
    if (vol > 100) vol = 100;
    float db = 0;
    for (int i = 1; i < 5; i++)
        if (vol <= P[i].v) {
            db = P[i - 1].db + (P[i].db - P[i - 1].db) * (vol - P[i - 1].v) / (P[i].v - P[i - 1].v);
            break;
        }
    return (int32_t)(32768.0f * powf(10.0f, db / 20.0f));
}

static void device_event(uac_host_device_handle_t h, const uac_host_device_event_t ev, void *arg)
{
    (void)arg;
    if (ev != UAC_HOST_DRIVER_EVENT_DISCONNECTED) return;
    uac_msg_t m = { .kind = 1, .h = h };
    xQueueSend(A.q, &m, 0);
}

static void driver_event(uint8_t addr, uint8_t iface, const uac_host_driver_event_t ev, void *arg)
{
    (void)arg;
    bool tx = ev == UAC_HOST_DRIVER_EVENT_TX_CONNECTED;
    if (!tx && ev != UAC_HOST_DRIVER_EVENT_RX_CONNECTED) return;
    uac_msg_t m = { .kind = 0, .addr = addr, .iface = iface, .tx = tx };
    xQueueSend(A.q, &m, 0);
}

static void card_gone(uac_host_device_handle_t h)
{
    xSemaphoreTake(A.mx, portMAX_DELAY);
    card_t *c = h == A.out.h ? &A.out : h == A.in.h ? &A.in : NULL;
    card_t old = c ? *c : (card_t){ 0 };
    if (c) memset(c, 0, sizeof *c);
    xSemaphoreGive(A.mx);
    if (!old.h) return;
    ESP_LOGI(TAG, "sound card \"%s\" %s gone", old.name, c == &A.out ? "output" : "input");
    uac_host_device_close(old.h);
}

/* A card's output or input appeared: opened, and its formats looked at. */
static void card_new(uint8_t addr, uint8_t iface, bool tx)
{
    card_t *c = tx ? &A.out : &A.in;
    if (c->h) return;               /* one of each */
    const uac_host_device_config_t cfg = { .addr = addr, .iface_num = iface, .buffer_size = RING_BYTES,
                                           .buffer_threshold = 0, .callback = device_event };
    uac_host_device_handle_t h;
    if (uac_host_device_open(&cfg, &h) != ESP_OK) {
        ESP_LOGW(TAG, "sound card at address %u: its %s did not open", addr, tx ? "output" : "input");
        return;
    }
    uac_host_dev_info_t info;
    card_t n = { .h = h, .addr = addr, .iface = iface };
    if (uac_host_get_device_info(h, &info) == ESP_OK) {
        char v[24], p[32];
        wide_to_str(info.iManufacturer, v, sizeof v);
        wide_to_str(info.iProduct, p, sizeof p);
        snprintf(n.name, sizeof n.name, "%s%s%s", v, v[0] && p[0] ? " " : "", p);
        /* 48 kHz 16-bit PCM in one of its alternate settings, two channels
         * if it has them */
        for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
            uac_host_dev_alt_param_t ap;
            if (uac_host_get_device_alt_param(h, alt, &ap) != ESP_OK || ap.bit_resolution != 16) continue;
            bool rate = false;
            if (ap.sample_freq_type == 0) rate = ap.sample_freq_lower <= RATE && RATE <= ap.sample_freq_upper;
            else
                for (int k = 0; k < ap.sample_freq_type && k < UAC_FREQ_NUM_MAX; k++) rate |= ap.sample_freq[k] == RATE;
            if (!rate || ap.channels < 1 || ap.channels > 2) continue;
            if (!n.ok || ap.channels == 2) n.channels = ap.channels;
            n.ok = true;
        }
    }
    if (!n.name[0]) snprintf(n.name, sizeof n.name, "USB sound card");
    xSemaphoreTake(A.mx, portMAX_DELAY);
    *c = n;
    xSemaphoreGive(A.mx);
    ESP_LOGI(TAG, "sound card \"%s\" %s at address %u: %s", n.name, tx ? "output" : "input", addr,
             n.ok ? (n.channels == 2 ? "48 kHz stereo" : "48 kHz mono") : "no 48 kHz 16-bit format: not used");
    aos_p4_usb_dev_use(addr, tx ? "speaker" : "microphone");
}

static void uac_task(void *arg)
{
    (void)arg;
    while (!A.stop) {
        uac_msg_t m;
        if (xQueueReceive(A.q, &m, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (m.kind == 0) card_new(m.addr, m.iface, m.tx);
        else card_gone(m.h);
    }
    A.done = true;
    vTaskDelete(NULL);
}

bool aos_p4_usb_uac_start(void)
{
    if (!A.mx) A.mx = xSemaphoreCreateMutex();
    if (!A.q) A.q = xQueueCreate(8, sizeof(uac_msg_t));
    memset(&A.out, 0, sizeof A.out);
    memset(&A.in, 0, sizeof A.in);
    A.vol = -1;
    A.stop = A.done = false;
    if (xTaskCreatePinnedToCore(uac_task, "usb_uac", 4096, NULL, 4, NULL, 0) != pdPASS) return false;
    const uac_host_driver_config_t cfg = { .create_background_task = true, .task_priority = 5, .stack_size = 4096,
                                           .core_id = 0, .callback = driver_event };
    esp_err_t e = uac_host_install(&cfg);
    A.installed = e == ESP_OK;
    if (!A.installed) {
        ESP_LOGW(TAG, "sound: uac_host_install: %s", esp_err_to_name(e));
        A.stop = true;
    }
    return A.installed;
}

void aos_p4_usb_uac_stop(void)
{
    if (!A.installed) return;
    A.stop = true;
    for (int i = 0; i < 50 && !A.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(A.mx, portMAX_DELAY);
    card_t o = A.out, i = A.in;
    memset(&A.out, 0, sizeof A.out);
    memset(&A.in, 0, sizeof A.in);
    xSemaphoreGive(A.mx);
    if (o.h) {
        if (o.streaming) uac_host_device_stop(o.h);
        uac_host_device_close(o.h);
    }
    if (i.h) uac_host_device_close(i.h);
    uac_host_uninstall();
    A.installed = false;
}

/* ---- for the board's audio output (aos_audio_p4.c) ------------------------ */

bool aos_hal_usb_audio_enabled(void)
{
    int32_t v = 1;
    aos_hal_pref_get_i32(PREF_AUDIO, &v);
    return v != 0;
}

void aos_hal_usb_audio_enable(bool on) { aos_hal_pref_set_i32(PREF_AUDIO, on); }

/* a card's output there, usable, and wanted */
bool aos_p4_usb_audio_out(void) { return A.installed && A.out.h && A.out.ok && aos_hal_usb_audio_enabled(); }

/* One 10 ms block of the mix, 48 kHz stereo, with the board's volume. */
void aos_p4_usb_audio_write(const int16_t *stereo, int frames, int volume)
{
    static int16_t buf[480 * 2];
    if (frames > 480) frames = 480;
    card_t *c = &A.out;
    if (!c->h || !c->ok) return;
    if (!c->streaming) {
        /* started outside the lock: it waits for control transfers */
        static uint32_t failed_at;
        if (failed_at && xTaskGetTickCount() - failed_at < pdMS_TO_TICKS(2000)) return;
        const uac_host_stream_config_t sc = { .channels = c->channels, .bit_resolution = 16, .sample_freq = RATE };
        if (uac_host_device_start(c->h, &sc) != ESP_OK) {
            failed_at = xTaskGetTickCount();
            ESP_LOGW(TAG, "sound card \"%s\": the stream did not start", c->name);
            return;
        }
        failed_at = 0;
        c->streaming = true;
        uac_host_device_set_volume(c->h, 100);      /* its own volume at the top: ours is in the samples */
        ESP_LOGI(TAG, "sound card \"%s\": playing", c->name);
    }
    xSemaphoreTake(A.mx, portMAX_DELAY);
    if (!c->h) {
        xSemaphoreGive(A.mx);
        return;
    }
    if (volume != A.vol) {
        A.vol = volume;
        A.gain = gain_of(volume);
    }
    int n;
    if (c->channels == 2) {
        for (int i = 0; i < frames * 2; i++) buf[i] = (int16_t)((stereo[i] * A.gain) >> 15);
        n = frames * 2;
    } else {
        for (int i = 0; i < frames; i++) buf[i] = (int16_t)(((stereo[2 * i] + stereo[2 * i + 1]) / 2 * A.gain) >> 15);
        n = frames;
    }
    if (uac_host_device_write(c->h, (uint8_t *)buf, n * sizeof(int16_t), 0) == ESP_OK) A.written++;
    else A.dropped++;
    xSemaphoreGive(A.mx);
}

/* the board's audio went idle: the card's stream stops too */
void aos_p4_usb_audio_idle(void)
{
    if (!A.installed) return;
    xSemaphoreTake(A.mx, portMAX_DELAY);
    uac_host_device_handle_t h = A.out.h && A.out.streaming ? A.out.h : NULL;
    xSemaphoreGive(A.mx);
    if (!h) return;
    uac_host_device_stop(h);        /* outside the lock: it waits for transfers */
    xSemaphoreTake(A.mx, portMAX_DELAY);
    if (A.out.h == h) {
        A.out.streaming = false;
        ESP_LOGI(TAG, "sound card \"%s\": stopped (%u blocks, %u dropped)", A.out.name, (unsigned)A.written,
                 (unsigned)A.dropped);
        A.written = A.dropped = 0;
    }
    xSemaphoreGive(A.mx);
}

bool aos_hal_usb_audio_info(char *name, size_t n, bool *out, bool *in, bool *playing)
{
    if (!A.installed || !A.mx) return false;
    xSemaphoreTake(A.mx, portMAX_DELAY);
    bool any = A.out.h || A.in.h;
    if (name) snprintf(name, n, "%s", A.out.h ? A.out.name : A.in.name);
    if (out) *out = A.out.h && A.out.ok;
    if (in) *in = A.in.h && A.in.ok;
    if (playing) *playing = A.out.streaming;
    xSemaphoreGive(A.mx);
    return any;
}
