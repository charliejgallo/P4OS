/*
 * RF - what the app keeps (rf.h): the audio as WAV in the recorder's
 * folder, the raw I/Q on the card, every decoded transmission in a CSV of
 * the day, and, when asked, each one published over MQTT.
 *
 * All of it is called from the engine's thread. The WAV (96 KB/s at most)
 * and the CSV (a line now and then) are written there, through a big stdio
 * buffer. The I/Q is up to 4.8 MB/s and the card stops for tens of
 * milliseconds now and then: it goes through a 4 MB ring in PSRAM to a
 * writer thread of its own, which empties it in 64 KB blocks from a buffer
 * the card's DMA takes (aos_hal_io_alloc); what does not fit in the ring is
 * counted, not waited for.
 */
#include "rf.h"
#include "rf_ook.h"
#include "rf_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "aos_hal.h"
/* the MQTT service (components/aos_apps/include/aos_mqtt.h), lent by the
 * firmware; its header is not in the apps' build */
int aos_mqtt_state(void);
bool aos_mqtt_publish(const char *topic, const char *payload, int qos, bool retain);
#define RF_MQTT_CONNECTED 5         /* AOS_MQTT_CONNECTED */

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

static void *psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

static const char *root(void)
{
    const char *r = aos_hal_path_sd_root();
    return r ? r : aos_hal_path_data();
}

static void stamp(char *out, size_t n)
{
    struct tm t;
    aos_hal_time_now(&t);
    if (aos_hal_time_is_valid())
        snprintf(out, n, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                 t.tm_sec);
    else snprintf(out, n, "up%llu", (unsigned long long)(aos_hal_uptime_ms() / 1000));
}

/* ---- WAV ------------------------------------------------------------------ */

static struct {
    FILE *f;
    uint32_t rate, samples;
    char *vbuf;
    char path[160];
} W;

static void wav_header(FILE *f, uint32_t rate, uint32_t samples)
{
    uint32_t bytes = samples * 2, x;
    uint16_t h;
    fwrite("RIFF", 1, 4, f);
    x = 36 + bytes; fwrite(&x, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    x = 16; fwrite(&x, 4, 1, f);
    h = 1; fwrite(&h, 2, 1, f);         /* PCM */
    h = 1; fwrite(&h, 2, 1, f);         /* mono */
    fwrite(&rate, 4, 1, f);
    x = rate * 2; fwrite(&x, 4, 1, f);
    h = 2; fwrite(&h, 2, 1, f);
    h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
}

bool rf_wav_start(uint32_t rate, uint32_t freq_hz)
{
    if (W.f) return true;
    const char *dir = aos_hal_path_recordings();
    mkdir(dir, 0777);
    char st[24];
    stamp(st, sizeof st);
    snprintf(W.path, sizeof W.path, "%s/RF_%s_%u.%03uMHz.wav", dir, st, (unsigned)(freq_hz / 1000000),
             (unsigned)(freq_hz % 1000000 / 1000));
    W.f = fopen(W.path, "wb");
    if (!W.f) return false;
    W.vbuf = psram(32 * 1024);
    if (W.vbuf) setvbuf(W.f, W.vbuf, _IOFBF, 32 * 1024);
    W.rate = rate;
    W.samples = 0;
    wav_header(W.f, rate, 0);
    aos_hal_log("rf", "recording audio to %s", W.path);
    return true;
}

void rf_wav_write(const int16_t *pcm, int n)
{
    if (!W.f || n <= 0) return;
    W.samples += (uint32_t)fwrite(pcm, sizeof(int16_t), n, W.f);
}

bool rf_wav_on(uint32_t *samples, uint32_t *rate)
{
    if (samples) *samples = W.samples;
    if (rate) *rate = W.rate;
    return W.f != NULL;
}

void rf_wav_stop(void)
{
    if (!W.f) return;
    fseek(W.f, 0, SEEK_SET);
    wav_header(W.f, W.rate, W.samples);
    fclose(W.f);
    W.f = NULL;
    free(W.vbuf);
    W.vbuf = NULL;
    aos_hal_log("rf", "audio recorded: %s, %u s", W.path, (unsigned)(W.samples / (W.rate ? W.rate : 1)));
}

/* ---- I/Q ------------------------------------------------------------------ */

#define IQ_RING (4u * 1024 * 1024)
#define IQ_BLOCK (64u * 1024)

static struct {
    volatile bool on, stop, done;
    uint8_t *ring;
    volatile uint32_t head, tail;   /* free-running: the engine writes head, the writer tail */
    FILE *f;
    volatile uint64_t written;
    volatile uint32_t dropped;
    char path[160];
} Q;

static void iq_writer(void *arg)
{
    (void)arg;
    uint8_t *blk = aos_hal_io_alloc(IQ_BLOCK);
    while (blk) {
        uint32_t used = Q.head - Q.tail;
        if (used < IQ_BLOCK && !Q.stop) {
            aos_hal_sleep_ms(10);
            continue;
        }
        if (!used) break;               /* stopped and empty */
        uint32_t n = used < IQ_BLOCK ? used : IQ_BLOCK;
        for (uint32_t k = 0; k < n;) {
            uint32_t at = (Q.tail + k) % IQ_RING, run = IQ_RING - at;
            if (run > n - k) run = n - k;
            memcpy(blk + k, Q.ring + at, run);
            k += run;
        }
        Q.tail += n;
        Q.written += fwrite(blk, 1, n, Q.f);
    }
    aos_hal_io_free(blk);
    Q.done = true;
}

bool rf_iq_start(uint32_t rate, uint32_t freq_hz, int gain)
{
    if (Q.on) return true;
    char dir[96], st[24];
    snprintf(dir, sizeof dir, "%s/rf", root());
    mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/rf/iq", root());
    mkdir(dir, 0777);
    stamp(st, sizeof st);
    /* the name says how to play it back: centre and rate */
    snprintf(Q.path, sizeof Q.path, "%s/%s_%u_%u.cu8", dir, st, (unsigned)freq_hz, (unsigned)rate);
    if (!Q.ring) Q.ring = psram(IQ_RING);
    if (!Q.ring) return false;
    Q.f = fopen(Q.path, "wb");
    if (!Q.f) return false;
    setvbuf(Q.f, NULL, _IONBF, 0);
    Q.head = Q.tail = 0;
    Q.written = 0;
    Q.dropped = 0;
    Q.stop = Q.done = false;
    if (!aos_hal_thread_start("rf_iq", iq_writer, NULL, 6 * 1024, 2)) {
        fclose(Q.f);
        Q.f = NULL;
        return false;
    }
    /* a note beside it, for people and for other programs */
    char note[180];
    snprintf(note, sizeof note, "%.*s.txt", (int)(strlen(Q.path) - 4), Q.path);
    FILE *t = fopen(note, "w");
    if (t) {
        fprintf(t, "format=cu8 (unsigned 8-bit I/Q, interleaved)\nfrequency_hz=%u\nrate_sps=%u\ngain_tenths_db=%d\n",
                (unsigned)freq_hz, (unsigned)rate, gain);
        fclose(t);
    }
    Q.on = true;
    aos_hal_log("rf", "recording I/Q to %s", Q.path);
    return true;
}

void rf_iq_write(const uint8_t *iq, int bytes)
{
    if (!Q.on || bytes <= 0) return;
    if (IQ_RING - (Q.head - Q.tail) < (uint32_t)bytes) {
        Q.dropped += bytes;         /* the card is behind: dropped, whole reads */
        return;
    }
    for (int k = 0; k < bytes;) {
        uint32_t at = (Q.head + k) % IQ_RING, run = IQ_RING - at;
        if (run > (uint32_t)(bytes - k)) run = bytes - k;
        memcpy(Q.ring + at, iq + k, run);
        k += run;
    }
    Q.head += bytes;
}

bool rf_iq_on(uint64_t *written, uint32_t *dropped)
{
    if (written) *written = Q.written;
    if (dropped) *dropped = Q.dropped;
    return Q.on;
}

void rf_iq_stop(void)
{
    if (!Q.on) return;
    Q.stop = true;
    for (int i = 0; i < 500 && !Q.done; i++) aos_hal_sleep_ms(10);
    fclose(Q.f);
    Q.f = NULL;
    Q.on = false;
    aos_hal_log("rf", "I/Q recorded: %s, %llu bytes, %u dropped", Q.path, (unsigned long long)Q.written,
                (unsigned)Q.dropped);
}

const char *rf_rec_last_path(bool iq)
{
    return iq ? Q.path : W.path;
}

/* ---- decoded transmissions ------------------------------------------------- */

void rf_log_decoded(const rf_decoded_t *d, uint32_t freq_hz, float snr_db, int32_t offset_hz)
{
    struct tm t;
    aos_hal_time_now(&t);
    char path[96];
    snprintf(path, sizeof path, "%s/rf", root());
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/rf/datos-%04d-%02d-%02d.csv", root(), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return;
    if (fresh)
        fprintf(f, "time,frequency_hz,offset_hz,snr_db,protocol,key,text,temperature_c,humidity,id,button,channel,"
                   "battery_low,modulation,short_us,long_us,bits,hex\n");
    static const char *const MOD[] = { "", "PWM", "PPM", "Manchester" };
    char temp[16] = "", hum[8] = "", btn[8] = "", batt[4] = "";
    if (d->has_temp) snprintf(temp, sizeof temp, "%.1f", (double)d->temp_c);
    if (d->has_hum) snprintf(hum, sizeof hum, "%d", d->hum);
    if (d->has_button) snprintf(btn, sizeof btn, "%d", d->button);
    if (d->has_batt) snprintf(batt, sizeof batt, "%d", d->batt_low ? 1 : 0);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%u,%d,%.1f,%s,%s,\"%s\",%s,%s,%u,%s,%d,%s,%s,%d,%d,%d,%s\n",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (unsigned)freq_hz,
            (int)offset_hz, (double)snr_db, d->proto, d->key, d->text, temp, hum, (unsigned)d->id, btn, d->channel, batt,
            MOD[d->mod], d->short_us, d->long_us, d->nbits, d->hex);
    fclose(f);
}

/* rf/lora-<day>.csv: a line a packet (the LoRa meter's, rf_lora.c) */
void rf_log_lora(const rf_lora_pkt_t *p, uint32_t freq_hz, const char *preset)
{
    struct tm t;
    aos_hal_time_now(&t);
    char path[96];
    snprintf(path, sizeof path, "%s/rf", root());
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/rf/lora-%04d-%02d-%02d.csv", root(), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return;
    if (fresh) fprintf(f, "time,frequency_hz,offset_hz,width_hz,duration_ms,snr_db,sf,bandwidth_hz,preset,quality\n");
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%u,%d,%u,%.1f,%.1f,%d,%u,%s,%.2f\n", t.tm_year + 1900, t.tm_mon + 1,
            t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (unsigned)freq_hz, (int)p->offset_hz, (unsigned)p->width_hz,
            p->dur_us / 1000.0, (double)p->snr_db, p->sf, (unsigned)p->bw_hz, preset ? preset : "", (double)p->quality);
    fclose(f);
}

bool rf_mqtt_ready(void)
{
    return aos_mqtt_state() == RF_MQTT_CONNECTED;
}

void rf_mqtt_decoded(const rf_decoded_t *d, uint32_t freq_hz, float snr_db)
{
    if (!rf_mqtt_ready()) return;
    char topic[96], json[400];
    snprintf(topic, sizeof topic, "%s/rf/%s", aos_hal_device_name(), d->key);
    int k = snprintf(json, sizeof json, "{\"protocol\":\"%s\",\"id\":%u,\"frequency_hz\":%u,\"snr_db\":%.1f",
                     d->proto[0] ? d->proto : "unknown", (unsigned)d->id, (unsigned)freq_hz, (double)snr_db);
    if (d->has_temp) k += snprintf(json + k, sizeof json - k, ",\"temperature_c\":%.1f", (double)d->temp_c);
    if (d->has_hum) k += snprintf(json + k, sizeof json - k, ",\"humidity\":%d", d->hum);
    if (d->channel) k += snprintf(json + k, sizeof json - k, ",\"channel\":%d", d->channel);
    if (d->has_batt) k += snprintf(json + k, sizeof json - k, ",\"battery_low\":%s", d->batt_low ? "true" : "false");
    if (d->has_button) k += snprintf(json + k, sizeof json - k, ",\"button\":%d", d->button);
    snprintf(json + k, sizeof json - k, ",\"bits\":%d,\"hex\":\"%s\"}", d->nbits, d->hex);
    aos_mqtt_publish(topic, json, 0, false);
}
