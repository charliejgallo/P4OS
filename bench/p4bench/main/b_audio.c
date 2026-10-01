/*
 * Audio (HARDWARE.md test 12). The ES8311 plays, the ES7210 records, and
 * both hang off the same I2S port in duplex (the BSP's setup), so the
 * interesting question is whether playing and recording at the same time
 * works - the watch could only do one or the other.
 *
 *   audio tone [hz] [ms] [vol]
 *   audio sweep [vol]           100 Hz -> 8 kHz in 6 s
 *   audio wav <path> [vol]      16-bit PCM WAV from the SD
 *   audio vol <0-100>
 *   mic [secs] [gain_db]        levels per second, WAV to /sdcard/bench/mic.wav
 *   duplex [secs]               tone out while recording: RMS silent vs playing
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "bench.h"

#define RATE 16000

static esp_codec_dev_handle_t s_spk, s_mic;
static int s_rate_open;
static bool s_spk_open, s_mic_open;

static bool codecs(void)
{
    if (!s_spk) s_spk = bsp_audio_codec_speaker_init();
    if (!s_mic) s_mic = bsp_audio_codec_microphone_init();
    return s_spk && s_mic;
}

static void spk_open(int rate, int ch)
{
    if (s_spk_open && s_rate_open == rate) return;
    if (s_spk_open) esp_codec_dev_close(s_spk);
    esp_codec_dev_sample_info_t fs = { .sample_rate = rate, .channel = ch, .bits_per_sample = 16 };
    esp_codec_dev_open(s_spk, &fs);
    esp_codec_dev_set_out_vol(s_spk, bench_cfg_get("vol", 60));
    s_spk_open = true;
    s_rate_open = rate;
}

static void mic_open(int gain_db)
{
    if (s_mic_open) esp_codec_dev_close(s_mic);
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 1, .bits_per_sample = 16 };
    esp_codec_dev_open(s_mic, &fs);
    esp_codec_dev_set_in_gain(s_mic, gain_db);
    s_mic_open = true;
}

static void tone(int hz, int ms, int vol)
{
    spk_open(RATE, 1);
    if (vol >= 0) esp_codec_dev_set_out_vol(s_spk, vol);
    int16_t buf[512];
    double ph = 0, step = 2 * M_PI * hz / RATE;
    int total = RATE * ms / 1000;
    for (int done = 0; done < total; done += 512) {
        for (int i = 0; i < 512; i++) { buf[i] = (int16_t)(sin(ph) * 12000); ph += step; }
        esp_codec_dev_write(s_spk, buf, sizeof buf);
    }
}

static void level(const int16_t *s, int n, double *rms_db, double *peak_db)
{
    double acc = 0;
    int peak = 0;
    for (int i = 0; i < n; i++) {
        acc += (double)s[i] * s[i];
        int a = abs(s[i]);
        if (a > peak) peak = a;
    }
    double rms = sqrt(acc / n);
    *rms_db = rms > 0 ? 20 * log10(rms / 32768.0) : -120;
    *peak_db = peak > 0 ? 20 * log10(peak / 32768.0) : -120;
}

static void wav_header(FILE *f, int rate, int ch, uint32_t data_bytes)
{
    uint32_t v;
    uint16_t w;
    fwrite("RIFF", 1, 4, f); v = 36 + data_bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    w = 1; fwrite(&w, 2, 1, f); w = ch; fwrite(&w, 2, 1, f);
    v = rate; fwrite(&v, 4, 1, f); v = rate * ch * 2; fwrite(&v, 4, 1, f);
    w = ch * 2; fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
}

static int cmd_audio(int argc, char **argv)
{
    if (!codecs()) { bench_report("audio", "error=codec_init"); return 0; }
    const char *sub = arg_str(argc, argv, 1, "tone");
    if (!strcmp(sub, "vol")) {
        int v = arg_int(argc, argv, 2, 60);
        bench_cfg_set("vol", v);
        if (s_spk_open) esp_codec_dev_set_out_vol(s_spk, v);
        bench_report("audio.vol", "vol=%d", v);
        return 0;
    }
    if (!strcmp(sub, "tone")) {
        int hz = arg_int(argc, argv, 2, 1000), ms = arg_int(argc, argv, 3, 1500), vol = arg_int(argc, argv, 4, -1);
        int64_t t = bench_us();
        tone(hz, ms, vol);
        bench_report("audio.tone", "hz=%d ms=%d took_ms=%lld", hz, ms, (bench_us() - t) / 1000);
        return 0;
    }
    if (!strcmp(sub, "sweep")) {
        spk_open(RATE, 1);
        int vol = arg_int(argc, argv, 2, -1);
        if (vol >= 0) esp_codec_dev_set_out_vol(s_spk, vol);
        int16_t buf[512];
        double ph = 0;
        int total = RATE * 6;
        for (int done = 0; done < total; done += 512) {
            for (int i = 0; i < 512; i++) {
                double f = 100 * pow(80.0, (double)(done + i) / total);   /* 100 Hz .. 8 kHz, log */
                ph += 2 * M_PI * f / RATE;
                buf[i] = (int16_t)(sin(ph) * 12000);
            }
            esp_codec_dev_write(s_spk, buf, sizeof buf);
        }
        bench_report("audio.sweep", "from_hz=100 to_hz=8000 secs=6");
        return 0;
    }
    if (!strcmp(sub, "wav")) {
        const char *path = arg_str(argc, argv, 2, "/sdcard/bench/test.wav");
        if (!sd_mounted()) sd_mount(40000);
        FILE *f = fopen(path, "rb");
        if (!f) { bench_report("audio.wav", "error=open path=%s", path); return 0; }
        uint8_t h[12];
        fread(h, 1, 12, f);
        int rate = 0, ch = 0, bits = 0;
        uint32_t data = 0;
        char id[4];
        uint32_t len;
        while (fread(id, 1, 4, f) == 4 && fread(&len, 4, 1, f) == 1) {
            if (!memcmp(id, "fmt ", 4)) {
                uint8_t fmt[16];
                fread(fmt, 1, 16, f);
                ch = fmt[2] | fmt[3] << 8;
                rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
                bits = fmt[14] | fmt[15] << 8;
                fseek(f, len - 16, SEEK_CUR);
            } else if (!memcmp(id, "data", 4)) { data = len; break; }
            else fseek(f, len, SEEK_CUR);
        }
        if (bits != 16 || !data) { bench_report("audio.wav", "error=format bits=%d", bits); fclose(f); return 0; }
        spk_open(rate, ch);
        int vol = arg_int(argc, argv, 3, -1);
        if (vol >= 0) esp_codec_dev_set_out_vol(s_spk, vol);
        uint8_t *buf = malloc(4096);
        int64_t t = bench_us();
        size_t n, played = 0;
        while (played < data && (n = fread(buf, 1, 4096, f)) > 0) { esp_codec_dev_write(s_spk, buf, n); played += n; }
        free(buf);
        fclose(f);
        bench_report("audio.wav", "path=%s rate=%d ch=%d bytes=%u secs=%.1f", path, rate, ch, (unsigned)played, (bench_us() - t) / 1e6);
        return 0;
    }
    printf("audio tone [hz] [ms] [vol] | sweep [vol] | wav <path> [vol] | vol <0-100>\n");
    return 0;
}

static int cmd_mic(int argc, char **argv)
{
    if (!codecs()) { bench_report("mic", "error=codec_init"); return 0; }
    int secs = arg_int(argc, argv, 1, 5), gain = arg_int(argc, argv, 2, 30);
    mic_open(gain);
    FILE *f = NULL;
    if (sd_mounted() || sd_mount(40000) == ESP_OK) {
        mkdir("/sdcard/bench", 0777);
        f = fopen("/sdcard/bench/mic.wav", "wb");
        if (f) wav_header(f, RATE, 1, 0);
    }
    const int n = RATE;             /* one second per block */
    int16_t *buf = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    bench_say("mic: recording %d s at gain %d dB - talk, clap, then stay quiet", secs, gain);
    double worst = -120, best = 0;
    for (int s = 0; s < secs; s++) {
        esp_codec_dev_read(s_mic, buf, n * 2);
        double rms, peak;
        level(buf, n, &rms, &peak);
        if (rms > worst) worst = rms;
        if (rms < best) best = rms;
        bench_report("mic.level", "second=%d rms_dbfs=%.1f peak_dbfs=%.1f", s + 1, rms, peak);
        if (f) fwrite(buf, 2, n, f);
    }
    if (f) {
        fseek(f, 0, SEEK_SET);
        wav_header(f, RATE, 1, secs * n * 2);
        fclose(f);
    }
    bench_report("mic", "secs=%d gain_db=%d loudest_rms=%.1f quietest_rms=%.1f wav=%s",
                 secs, gain, worst, best, f ? "/sdcard/bench/mic.wav" : "none");
    free(buf);
    return 0;
}

static volatile bool s_duplex_play;
static void duplex_tone_task(void *arg)
{
    int16_t buf[512];
    double ph = 0, step = 2 * M_PI * 1000 / RATE;
    while (s_duplex_play) {
        for (int i = 0; i < 512; i++) { buf[i] = (int16_t)(sin(ph) * 12000); ph += step; }
        esp_codec_dev_write(s_spk, buf, sizeof buf);
    }
    vTaskDelete(NULL);
}

static int cmd_duplex(int argc, char **argv)
{
    if (!codecs()) { bench_report("duplex", "error=codec_init"); return 0; }
    int secs = arg_int(argc, argv, 1, 3);
    spk_open(RATE, 1);
    mic_open(24);
    const int n = RATE / 2;
    int16_t *buf = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    double quiet = 0, loud = 0, rms, peak;
    for (int i = 0; i < 2; i++) { esp_codec_dev_read(s_mic, buf, n * 2); level(buf, n, &rms, &peak); quiet += rms / 2; }
    s_duplex_play = true;
    xTaskCreatePinnedToCore(duplex_tone_task, "duplex", 4096, NULL, 6, NULL, 0);
    int blocks = secs * 2, ok_reads = 0;
    int64_t t = bench_us();
    for (int i = 0; i < blocks; i++) {
        if (esp_codec_dev_read(s_mic, buf, n * 2) == ESP_CODEC_DEV_OK) ok_reads++;
        level(buf, n, &rms, &peak);
        loud += rms / blocks;
    }
    double took = (bench_us() - t) / 1e6;
    s_duplex_play = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    free(buf);
    bench_report("duplex", "secs=%d quiet_rms=%.1f playing_rms=%.1f delta_db=%.1f reads_ok=%d/%d realtime=%.2f works=%d",
                 secs, quiet, loud, loud - quiet, ok_reads, blocks, took / secs, loud - quiet > 10 && ok_reads == blocks);
    return 0;
}

void reg_audio(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "audio", .help = "speaker: tone sweep wav vol", .func = cmd_audio },
        { .command = "mic", .help = "mic [secs] [gain_db]: levels + WAV on the SD", .func = cmd_mic },
        { .command = "duplex", .help = "duplex [secs]: record while playing (test 12)", .func = cmd_duplex },
    };
    for (int i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
}
