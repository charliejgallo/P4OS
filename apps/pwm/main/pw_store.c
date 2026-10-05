/*
 * P4OS - PWM generator: the files on the card, in /sdcard/pwm.
 *
 *   <name>.pwm     a preset: the channels, one line each
 *   _estado.txt    what the app has now, for the portal's page: written
 *                  a moment after every change, with what the hardware gave
 *                  (real frequency, bits, the error if a channel would not
 *                  open) and run=1 while the app is open
 *   _pedido.txt    what the page wants: the whole set of channels, in the
 *                  same format. The app takes it within 300 ms, applies it
 *                  and deletes it; ack= in the state says which one it took.
 *                  A whole set and not a change, so that a newer request
 *                  simply replaces one not yet taken.
 *
 * The format is a line per channel of key=value pairs, easy to write by
 * hand and from JavaScript:
 *
 *   pwm 1 run=1 ack=12
 *   ch gpio=28 mode=servo on=1 inv=0 freq=1000 duty=0.5000 pulse=1500 ...
 *
 * err= goes last and takes the rest of the line. A file's date is no good
 * to notice a change (FAT keeps it to two seconds), hence the delete.
 */
#include "pw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const MODE_KEYS[PW_M_COUNT] = { "pwm", "servo", "led", "nivel" };
static const char *const PAT_KEYS[PW_P_COUNT] = { "fijo", "respirar", "barrido", "estrobo", "rampa", "seno", "pasos" };

void pw_store_dir(char *out, size_t n)
{
    const char *r = aos_hal_path_sd_root();
    if (!r) { out[0] = 0; return; }
    snprintf(out, n, "%s/pwm", r);
}

static int key_index(const char *v, const char *const *keys, int n, int def)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, keys[i])) return i;
    return def;
}

static size_t cfg_line(char *b, size_t n, const pw_ch_t *h, bool extra)
{
    const pw_cfg_t *c = &h->c;
    int k = snprintf(b, n,
                     "ch gpio=%d mode=%s on=%d inv=%d freq=%u duty=%.4f pulse=%.0f smin=%u smax=%u sdeg=%u "
                     "bright=%.4f gamma=%.2f level=%.4f pat=%s period=%.3f lo=%.4f hi=%.4f steps=",
                     c->gpio, MODE_KEYS[c->mode < PW_M_COUNT ? c->mode : 0], (extra ? (h->pwm || h->dac) : c->on) ? 1 : 0,
                     c->invert ? 1 : 0, (unsigned)c->freq, (double)c->duty, (double)c->pulse, c->smin, c->smax, c->sdeg,
                     (double)c->bright, (double)c->gamma, (double)c->level, PAT_KEYS[c->pat < PW_P_COUNT ? c->pat : 0],
                     (double)c->period, (double)c->lo, (double)c->hi);
    for (int i = 0; i < c->nsteps && k > 0 && (size_t)k < n; i++)
        k += snprintf(b + k, n - (size_t)k, "%s%.3f", i ? "," : "", (double)c->step[i]);
    if (extra && k > 0 && (size_t)k < n) {
        k += snprintf(b + k, n - (size_t)k, " rfreq=%u bits=%d live=%.4f err=%s",
                      (unsigned)(h->pwm ? aos_io_pwm_freq(h->pwm) : 0), h->pwm ? aos_io_pwm_bits(h->pwm) : 0,
                      (double)h->live, h->err);
    }
    if (k > 0 && (size_t)k + 1 < n) { b[k++] = '\n'; b[k] = 0; }
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

static bool write_file(const char *path, bool extra, bool run)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    char line[600];
    if (extra) snprintf(line, sizeof line, "pwm 1 run=%d ack=%u sel=%d\n", run ? 1 : 0, (unsigned)pw_g->ack, pw_g->sel);
    else snprintf(line, sizeof line, "# P4OS, PWM generator (aos.pwm)\npwm 1\n");
    bool ok = fputs(line, f) >= 0;
    for (int i = 0; i < pw_g->n && ok; i++) {
        if (cfg_line(line, sizeof line, &pw_g->ch[i], extra)) ok = fputs(line, f) >= 0;
    }
    ok = fclose(f) == 0 && ok;
    return ok;
}

bool pw_store_save(const char *path) { return write_file(path, false, false); }

static float fclamp(float v, float a, float b, float def)
{
    if (!(v >= a && v <= b)) return v > b ? b : v < a ? a : def;
    return v;
}

static void cfg_check(pw_cfg_t *c)
{
    const aos_io_pin_t *p = c->gpio >= 0 ? aos_io_pin_of_gpio(c->gpio) : NULL;
    if (!p || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) c->gpio = -1;
    if (c->gpio < 0) c->on = false;
    if (c->freq < 1) c->freq = 1;
    if (c->freq > PW_FREQ_MAX) c->freq = PW_FREQ_MAX;
    c->duty = fclamp(c->duty, 0, 1, 0.5f);
    if (c->smin < 200 || c->smin > 2900) c->smin = 500;
    if (c->smax < c->smin + 100 || c->smax > 3000) c->smax = c->smin + 2000 > 3000 ? 3000 : c->smin + 2000;
    if (c->sdeg < 10 || c->sdeg > 360) c->sdeg = 180;
    c->pulse = fclamp(c->pulse, c->smin, c->smax, (float)(c->smin + c->smax) / 2);
    c->bright = fclamp(c->bright, 0, 1, 0.5f);
    c->gamma = fclamp(c->gamma, 1.0f, 3.0f, 2.2f);
    c->level = fclamp(c->level, 0, 1, 0.5f);
    c->period = fclamp(c->period, PW_PERIOD_MIN, PW_PERIOD_MAX, 2.0f);
    c->lo = fclamp(c->lo, 0, 1, 0);
    c->hi = fclamp(c->hi, 0, 1, 1);
    if (c->lo > c->hi) { float t = c->lo; c->lo = c->hi; c->hi = t; }
    if (c->nsteps < 1 || c->nsteps > PW_STEPS_MAX) c->nsteps = 4;
    for (int i = 0; i < PW_STEPS_MAX; i++) c->step[i] = fclamp(c->step[i], 0, 1, 0);
}

static void set_key(pw_cfg_t *c, const char *k, const char *v)
{
    if (!strcmp(k, "gpio")) c->gpio = (int8_t)strtol(v, NULL, 10);
    else if (!strcmp(k, "mode")) c->mode = (uint8_t)key_index(v, MODE_KEYS, PW_M_COUNT, PW_M_PWM);
    else if (!strcmp(k, "on")) c->on = v[0] == '1';
    else if (!strcmp(k, "inv")) c->invert = v[0] == '1';
    else if (!strcmp(k, "freq")) c->freq = (uint32_t)strtol(v, NULL, 10);
    else if (!strcmp(k, "duty")) c->duty = strtof(v, NULL);
    else if (!strcmp(k, "pulse")) c->pulse = strtof(v, NULL);
    else if (!strcmp(k, "smin")) c->smin = (uint16_t)strtol(v, NULL, 10);
    else if (!strcmp(k, "smax")) c->smax = (uint16_t)strtol(v, NULL, 10);
    else if (!strcmp(k, "sdeg")) c->sdeg = (uint16_t)strtol(v, NULL, 10);
    else if (!strcmp(k, "bright")) c->bright = strtof(v, NULL);
    else if (!strcmp(k, "gamma")) c->gamma = strtof(v, NULL);
    else if (!strcmp(k, "level")) c->level = strtof(v, NULL);
    else if (!strcmp(k, "pat")) c->pat = (uint8_t)key_index(v, PAT_KEYS, PW_P_COUNT, PW_P_NONE);
    else if (!strcmp(k, "period")) c->period = strtof(v, NULL);
    else if (!strcmp(k, "lo")) c->lo = strtof(v, NULL);
    else if (!strcmp(k, "hi")) c->hi = strtof(v, NULL);
    else if (!strcmp(k, "steps")) {
        int n = 0;
        const char *p = v;
        while (*p && n < PW_STEPS_MAX) {
            char *end;
            float x = strtof(p, &end);
            if (end == p) break;
            c->step[n++] = x;
            p = *end == ',' ? end + 1 : end;
        }
        if (n) c->nsteps = (uint8_t)n;
    }
}

int pw_store_load(const char *path, pw_cfg_t *out, int max, uint32_t *seq)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[640];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "pwm", 3)) {
            const char *s = strstr(line, " seq=");
            if (s && seq) *seq = (uint32_t)strtol(s + 5, NULL, 10);
            continue;
        }
        if (strncmp(line, "ch ", 3) || n >= max) continue;
        pw_cfg_t *c = &out[n++];
        pw_cfg_default(c, PW_M_PWM);
        char *p = line + 3;
        while (*p) {
            while (*p == ' ') p++;
            char *eq = strchr(p, '=');
            if (!eq) break;
            *eq = 0;
            char *k = p, *v = eq + 1;
            if (!strcmp(k, "err")) break;                 /* the rest of the line */
            char *sp = strchr(v, ' ');
            if (sp) *sp = 0;
            set_key(c, k, v);
            if (!sp) break;
            p = sp + 1;
        }
        cfg_check(c);
    }
    fclose(f);
    return n;
}

void pw_store_changed(void)
{
    pw_g->save_due = (lv_tick_get() + 400) | 1;
}

static void state_path(char *out, size_t n, const char *file)
{
    char dir[128];
    pw_store_dir(dir, sizeof dir);
    if (!dir[0]) { out[0] = 0; return; }
    snprintf(out, n, "%s/%s", dir, file);
}

static void write_state(bool run)
{
    char dir[128], p[200];
    pw_store_dir(dir, sizeof dir);
    if (!dir[0]) return;
    mkdir(dir, 0777);
    state_path(p, sizeof p, "_estado.txt");
    if (!write_file(p, true, run)) aos_hal_log("pwm", "could not write %s", p);
}

void pw_store_restore(void)
{
    char p[200];
    state_path(p, sizeof p, "_estado.txt");
    if (!p[0]) return;
    pw_cfg_t cfg[PW_CH_MAX];
    int n = pw_store_load(p, cfg, PW_CH_MAX, NULL);
    if (n <= 0) return;
    /* the outputs stopped when the app closed: they come back off */
    for (int i = 0; i < n; i++) cfg[i].on = false;
    pw_apply(cfg, n);
    pw_g->save_due = 0;
}

void pw_store_poll(void)
{
    char p[200];
    state_path(p, sizeof p, "_pedido.txt");
    if (!p[0]) return;
    struct stat st;
    if (stat(p, &st) == 0) {
        pw_cfg_t cfg[PW_CH_MAX];
        uint32_t seq = 0;
        int n = pw_store_load(p, cfg, PW_CH_MAX, &seq);
        remove(p);
        if (n > 0) {
            if (pw_g->sheet) pw_sheet_close();
            pw_apply(cfg, n);
            pw_g->ack = seq;
            pw_ui_refresh();
            aos_hal_log("pwm", "the portal's request %u: %d channels", (unsigned)seq, n);
        }
        pw_g->save_due = lv_tick_get() | 1;
    }
    if (pw_g->save_due && (int32_t)(lv_tick_get() - pw_g->save_due) >= 0) {
        pw_g->save_due = 0;
        write_state(true);
    }
}

void pw_store_final(void)
{
    write_state(false);
}
