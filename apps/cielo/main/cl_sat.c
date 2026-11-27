/*
 * CIELO - the satellites (see cielo.h).
 *
 * The elements come from a free service (CelesTrak's "visual" group by
 * default: the hundred-odd brightest, the stations among them) as plain
 * TLE text, and are kept in <data>/cielo_tle.txt with the time they were
 * fetched on its first line ("# 1764240000 <url>"). They are fetched again
 * when they are a day old, never more than once in two hours unless asked
 * (CelesTrak asks its users not to hammer it), and the app works from the
 * file while offline, saying how old it is.
 *
 * The passes are worked out in a thread of their own (cl_passes, SGP4 in
 * soft double: about a second per satellite and three days on the board),
 * from the shown time, and written for the portal page to
 * <data>/cielo_pasadas.json. <data>/cielo_ahora.json is the sky of the
 * moment (the bodies and the satellites up, and the sidereal time the page
 * needs to place the pack's stars), written every half minute while the
 * app is open.
 *
 * An alert goes up alert_min minutes before each visible pass, as a toast
 * with the system's notification sound. While the app is hidden it keeps
 * ticking for that (AOS_APP_FLAG_BACKGROUND, only while alerts are on);
 * its words were translated when it was in front, because _() reads the
 * catalogue of the app in front (HANDOFF-APPS.md, section 8).
 */
#include "cielo.h"
#include "cl_dmath.h"
#include "cl_mem.h"

#include "aos_i18n.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TLE_MAX_BYTES   (512 * 1024)
#define TLE_MAX_LIST    400
#define PASS_DAYS       3.0
#define AUTO_AGE_S      (24 * 3600)
#define RETRY_MS        (2 * 3600 * 1000u)

static void tle_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cielo_tle.txt", aos_hal_path_data());
}

/* brightness at 1000 km, half lit; the default for the rest */
float cl_sat_std_mag(uint32_t norad)
{
    switch (norad) {
    case 25544: return -1.8f;   /* ISS */
    case 48274: return -0.8f;   /* Tianhe, China's station */
    case 20580: return 2.2f;    /* Hubble */
    default: return 4.0f;
    }
}

int cl_sat_find(const app_t *a, uint32_t norad)
{
    for (int i = 0; i < a->nsats; i++)
        if (a->sats[i].el.norad == norad) return i;
    return -1;
}

/* the file, parsed: the chosen satellites into a->sats, every name into the
 * list the panel shows */
void cl_sat_reload(app_t *a)
{
    char p[160];
    tle_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    cl_satx_t tmp[CL_MAX_SATS];
    int n = 0;
    a->tle_n = 0;
    a->tle_time = 0;
    if (!a->tle_names) a->tle_names = (char(*)[25])cl_malloc(25 * TLE_MAX_LIST);
    if (!a->tle_ids) a->tle_ids = (uint32_t *)cl_malloc(sizeof(uint32_t) * TLE_MAX_LIST);
    if (f) {
        char l0[96] = "", l1[96], l2[96];
        char ln[160];
        int stage = 0;
        while (fgets(ln, sizeof ln, f)) {
            size_t k = strlen(ln);
            while (k && (ln[k - 1] == '\n' || ln[k - 1] == '\r' || ln[k - 1] == ' ')) ln[--k] = 0;
            if (ln[0] == '#') {
                long t = strtol(ln + 1, NULL, 10);
                if (t > 0) a->tle_time = t;
                continue;
            }
            if (!k) continue;
            if (ln[0] == '1' && ln[1] == ' ' && k >= 69) {
                snprintf(l1, sizeof l1, "%.90s", ln);
                stage = 1;
            } else if (ln[0] == '2' && ln[1] == ' ' && k >= 69 && stage == 1) {
                snprintf(l2, sizeof l2, "%.90s", ln);
                cl_sat_t s;
                if (cl_tle_parse(&s, l0[0] ? l0 : NULL, l1, l2) || s.norad) {
                    if (!s.name[0]) snprintf(s.name, sizeof s.name, "%u", (unsigned)s.norad);
                    if (a->tle_n < TLE_MAX_LIST && a->tle_names && a->tle_ids) {
                        snprintf(a->tle_names[a->tle_n], 25, "%s", s.name);
                        a->tle_ids[a->tle_n++] = s.norad;
                    }
                    for (int i = 0; i < a->conf.nsat_ids && n < CL_MAX_SATS; i++) {
                        if (a->conf.sat_ids[i] == s.norad && s.ok) {
                            tmp[n].el = s;
                            tmp[n].std_mag = cl_sat_std_mag(s.norad);
                            n++;
                        }
                    }
                }
                stage = 0;
                l0[0] = 0;
            } else {
                snprintf(l0, sizeof l0, "%.90s", ln);
                stage = 0;
            }
        }
        fclose(f);
    }
    aos_hal_mutex_lock(a->mutex);
    memcpy(a->sats, tmp, sizeof(cl_satx_t) * (size_t)n);
    a->nsats = n;
    a->sats_gen++;
    a->npasses = 0;
    aos_hal_mutex_unlock(a->mutex);
    aos_hal_log("cielo", "TLE: %d in the file, %d chosen and good, fetched %ld", a->tle_n, n, a->tle_time);
}

void cl_sat_fetch(app_t *a, bool manual)
{
    if (a->tle_req > 0) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!manual && a->tle_last_try_ms && now - a->tle_last_try_ms < RETRY_MS) return;
    if (manual && a->tle_last_try_ms && now - a->tle_last_try_ms < 30000) return;
    a->tle_last_try_ms = now;
    a->tle_manual = manual;
    a->tle_req = aos_hal_http_get(a->conf.tle_url, TLE_MAX_BYTES);
    a->tle_req_ms = now;
    if (a->tle_req <= 0) {
        snprintf(a->tle_err, sizeof a->tle_err, "%s (%d)", _("Sin red"), a->tle_req);
        aos_hal_log("cielo", "TLE request refused: %d", a->tle_req);
        a->tle_req = 0;
    } else {
        aos_hal_log("cielo", "TLE: asking %s", a->conf.tle_url);
        a->tle_err[0] = 0;
    }
}

static void fetch_poll(app_t *a)
{
    if (a->tle_req <= 0) return;
    aos_http_state_t st = aos_hal_http_state(a->tle_req);
    if (st == AOS_HTTP_BUSY) return;
    int code = aos_hal_http_status(a->tle_req);
    if (st == AOS_HTTP_DONE) {
        const char *b = aos_hal_http_body(a->tle_req);
        int len = aos_hal_http_len(a->tle_req);
        /* a TLE file has lines that begin "1 " and "2 "; an error page does not */
        bool good = b && len > 140 && (strstr(b, "\n1 ") || !strncmp(b, "1 ", 2)) && strstr(b, "\n2 ");
        if (good) {
            char p[160], tmp[170];
            tle_path(p, sizeof p);
            snprintf(tmp, sizeof tmp, "%s.part", p);
            FILE *f = fopen(tmp, "w");
            if (f) {
                fprintf(f, "# %ld %s\n", (long)time(NULL), a->conf.tle_url);
                fwrite(b, 1, (size_t)len, f);
                fclose(f);
                remove(p);
                rename(tmp, p);
            }
            aos_hal_log("cielo", "TLE: %d bytes fetched", len);
            cl_sat_reload(a);
            cl_sat_replan(a);
            if (a->tle_manual) aos_ui_toast(_("Órbitas actualizadas"), 1800);
        } else {
            snprintf(a->tle_err, sizeof a->tle_err, "%s", _("La respuesta no trae órbitas"));
            aos_hal_log("cielo", "TLE: an answer without elements (%d bytes)", len);
        }
    } else {
        snprintf(a->tle_err, sizeof a->tle_err, "%s (%d)", _("No se pudieron bajar las órbitas"), code);
        aos_hal_log("cielo", "TLE: failed, %d", code);
        if (a->tle_manual) aos_ui_toast(a->tle_err, 2500);
    }
    aos_hal_http_release(a->tle_req);
    a->tle_req = 0;
}

/* ---------------------------------------------------------------------------
 * The passes, in a thread
 * ------------------------------------------------------------------------- */

static int cmp_pass(const void *x, const void *y)
{
    const cl_passx_t *a = (const cl_passx_t *)x, *b = (const cl_passx_t *)y;
    return a->p.rise < b->p.rise ? -1 : a->p.rise > b->p.rise ? 1 : 0;
}

static void pass_main(void *arg)
{
    app_t *a = (app_t *)arg;
    cl_satx_t *sats = (cl_satx_t *)cl_malloc(sizeof(cl_satx_t) * CL_MAX_SATS);
    cl_passx_t *out = (cl_passx_t *)cl_malloc(sizeof(cl_passx_t) * CL_MAX_PASSES);
    cl_pass_t *tmp = (cl_pass_t *)cl_malloc(sizeof(cl_pass_t) * CL_MAX_PASSES);
    while (!a->pass_stop && sats && out && tmp) {
        if (!a->pass_redo) {
            aos_hal_sleep_ms(100);
            continue;
        }
        a->pass_redo = false;
        a->pass_busy = true;
        aos_hal_mutex_lock(a->mutex);
        int ns = a->nsats;
        memcpy(sats, a->sats, sizeof(cl_satx_t) * (size_t)ns);
        uint32_t gen = a->sats_gen;
        double jd0 = a->pass_jd;
        float lat = a->conf.lat, lon = a->conf.lon, h = a->conf.height / 1000.0f;
        aos_hal_mutex_unlock(a->mutex);
        uint64_t t0 = aos_hal_uptime_ms();
        int n = 0;
        for (int k = 0; k < ns && !a->pass_stop && !a->pass_redo; k++) {
            int got = cl_passes(&sats[k].el, jd0, PASS_DAYS, lat, lon, h, 10.0f * (float)CL_D2R, sats[k].std_mag,
                                tmp, CL_MAX_PASSES, &a->pass_stop);
            for (int i = 0; i < got && n < CL_MAX_PASSES; i++) {
                out[n].sat = k;
                out[n].p = tmp[i];
                out[n].alerted = false;
                n++;
            }
        }
        if (a->pass_stop) break;
        if (a->pass_redo) continue;
        qsort(out, (size_t)n, sizeof(cl_passx_t), cmp_pass);
        aos_hal_mutex_lock(a->mutex);
        if (gen == a->sats_gen) {
            /* keep what was already alerted */
            for (int i = 0; i < n; i++)
                for (int j = 0; j < a->npasses; j++)
                    if (a->passes[j].alerted && a->passes[j].sat == out[i].sat &&
                        cl_dabs(a->passes[j].p.rise - out[i].p.rise) < 60.0 / 86400)
                        out[i].alerted = true;
            memcpy(a->passes, out, sizeof(cl_passx_t) * (size_t)n);
            a->npasses = n;
        }
        aos_hal_mutex_unlock(a->mutex);
        aos_hal_log("cielo", "passes: %d for %d satellites in %u ms", n, ns, (unsigned)(aos_hal_uptime_ms() - t0));
        a->pass_busy = false;
        a->json_ms = 0;         /* write them for the page */
    }
    cl_free(sats);
    cl_free(out);
    cl_free(tmp);
    a->pass_busy = false;
    a->pass_thread = false;
}

void cl_sat_replan(app_t *a)
{
    a->pass_jd = cl_now_jd(a) - 10.0 / 1440;
    a->pass_redo = true;
}

void cl_sat_init(app_t *a)
{
    cl_sat_reload(a);
    a->pass_thread = true;
    a->pass_stop = false;
    if (!aos_hal_thread_start("cielo_pass", pass_main, a, 16384, 2)) {
        a->pass_thread = false;
        aos_hal_log("cielo", "no thread for the passes");
    }
    cl_sat_replan(a);
}

void cl_sat_free(app_t *a)
{
    a->pass_stop = true;
    for (int i = 0; i < 400 && a->pass_thread; i++) aos_hal_sleep_ms(10);
    if (a->pass_thread) aos_hal_log("cielo", "the passes thread did not stop");
    if (a->tle_req > 0) aos_hal_http_release(a->tle_req);
    a->tle_req = 0;
    cl_free(a->tle_names);
    cl_free(a->tle_ids);
    a->tle_names = NULL;
    a->tle_ids = NULL;
}

/* ---------------------------------------------------------------------------
 * The files for the page
 * ------------------------------------------------------------------------- */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

static long unix_of(double jd)
{
    return jd > 0 ? (long)cl_unix_from_jd(jd) : 0;
}

static void write_passes(app_t *a)
{
    char p[160], tmp[170];
    snprintf(p, sizeof p, "%s/cielo_pasadas.json", aos_hal_path_data());
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"place\":");
    json_str(f, a->conf.place);
    fprintf(f, ",\"lat\":%.4f,\"lon\":%.4f,\"made\":%ld,\"tle_time\":%ld,\"from\":%ld,\"passes\":[", (double)a->conf.lat,
            (double)a->conf.lon, (long)time(NULL), a->tle_time, unix_of(a->pass_jd));
    aos_hal_mutex_lock(a->mutex);
    for (int i = 0; i < a->npasses; i++) {
        const cl_passx_t *x = &a->passes[i];
        const cl_pass_t *q = &x->p;
        if (x->sat >= a->nsats) continue;
        fprintf(f, "%s{\"sat\":", i ? "," : "");
        json_str(f, a->sats[x->sat].el.name);
        fprintf(f,
                ",\"norad\":%u,\"rise\":%ld,\"top\":%ld,\"set\":%ld,\"rise_az\":%.0f,\"top_az\":%.0f,\"set_az\":%.0f,"
                "\"top_el\":%.0f,\"vis_start\":%ld,\"vis_end\":%ld,\"mag\":%.1f}",
                (unsigned)a->sats[x->sat].el.norad, unix_of(q->rise), unix_of(q->top), unix_of(q->set),
                (double)(q->rise_az * 57.29578f), (double)(q->top_az * 57.29578f), (double)(q->set_az * 57.29578f),
                (double)(q->top_el * 57.29578f), unix_of(q->vis_start), unix_of(q->vis_end),
                (double)(q->mag < 90 ? q->mag : 99));
    }
    aos_hal_mutex_unlock(a->mutex);
    fputs("]}\n", f);
    fclose(f);
    remove(p);
    rename(tmp, p);
}

void cl_sat_write_now(app_t *a, double jd)
{
    char p[160], tmp[170];
    snprintf(p, sizeof p, "%s/cielo_ahora.json", aos_hal_path_data());
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    cl_sky_t sky;
    cl_sky_set(&sky, jd, a->conf.lat, a->conf.lon);
    fprintf(f, "{\"time\":%ld,\"lat\":%.4f,\"lon\":%.4f,\"lst\":%.4f,\"night\":%d,\"place\":", unix_of(jd),
            (double)a->conf.lat, (double)a->conf.lon, sky.lst * CL_R2D, a->conf.night ? 1 : 0);
    json_str(f, a->conf.place);
    fputs(",\"bodies\":[", f);
    for (int b = 0; b < CL_NBODY; b++) {
        cl_body_t bp;
        cl_body(b, jd, &bp);
        float enu[3], alt, az;
        cl_radec_to_enu(&sky, bp.ra, bp.dec, b == CL_MOON ? bp.dist_au : 0, enu);
        cl_enu_altaz(enu, &alt, &az);
        fprintf(f, "%s{\"id\":%d,\"name\":", b ? "," : "", b);
        json_str(f, cl_body_name(b));
        fprintf(f, ",\"alt\":%.2f,\"az\":%.2f,\"mag\":%.1f,\"phase\":%.2f,\"ra\":%.3f,\"dec\":%.3f}",
                (double)(alt * 57.29578f), (double)(az * 57.29578f), (double)bp.mag, (double)bp.phase,
                bp.ra * CL_R2D, bp.dec * CL_R2D);
    }
    fputs("],\"sats\":[", f);
    aos_hal_mutex_lock(a->mutex);
    int w = 0;
    for (int k = 0; k < a->nsats; k++) {
        cl_look_t lk;
        if (!cl_sat_look(&a->sats[k].el, jd, a->conf.lat, a->conf.lon, 0.02, a->sats[k].std_mag, &lk)) continue;
        fprintf(f, "%s{\"name\":", w++ ? "," : "");
        json_str(f, a->sats[k].el.name);
        fprintf(f, ",\"alt\":%.2f,\"az\":%.2f,\"lit\":%d,\"range\":%.0f,\"sublat\":%.2f,\"sublon\":%.2f}",
                lk.el * CL_R2D, lk.az * CL_R2D, lk.sunlit ? 1 : 0, lk.range, lk.lat, lk.lon);
    }
    aos_hal_mutex_unlock(a->mutex);
    fputs("]}\n", f);
    fclose(f);
    remove(p);
    rename(tmp, p);
}

/* ---------------------------------------------------------------------------
 * The tick
 * ------------------------------------------------------------------------- */

static const char *compass(float az)
{
    static const char *const P[8] = { N_("N"), N_("NE"), N_("E"), N_("SE"), N_("S"), N_("SO"), N_("O"), N_("NO") };
    int i = (int)floorf(az * 57.29578f / 45.0f + 0.5f) & 7;
    return P[i];
}

void cl_sat_tick(app_t *a, double jd_now, uint32_t now_ms)
{
    fetch_poll(a);
    /* the elements a day old: again, quietly */
    long age = a->tle_time ? (long)time(NULL) - a->tle_time : -1;
    if (aos_hal_time_is_valid() && (age < 0 || age > AUTO_AGE_S) && a->conf.nsat_ids && a->conf.sats &&
        aos_hal_net_state() == AOS_NET_CONNECTED)
        cl_sat_fetch(a, false);

    /* the shown time ran off the passes worked out: again */
    if (!a->pass_busy && !a->pass_redo && (jd_now < a->pass_jd || jd_now > a->pass_jd + PASS_DAYS - 0.5))
        cl_sat_replan(a);

    /* the alerts, against the real clock */
    if (a->conf.alerts && aos_hal_time_is_valid()) {
        double real = cl_real_jd();
        aos_hal_mutex_lock(a->mutex);
        for (int i = 0; i < a->npasses; i++) {
            cl_passx_t *x = &a->passes[i];
            if (x->alerted || !x->p.vis_start || x->sat >= a->nsats) continue;
            double lead = (x->p.vis_start - real) * 1440.0;
            if (lead <= a->conf.alert_min && lead > -1.0) {
                x->alerted = true;
                char hm[16];
                cl_fmt_time(x->p.vis_start, hm, sizeof hm, 0);
                /* the format was translated in front (alert_fmt); the
                 * compass point stays as the catalogue had it */
                snprintf(a->alert_txt, sizeof a->alert_txt, a->alert_fmt ? a->alert_fmt : "%s %s %s %d",
                         a->sats[x->sat].el.name, hm, a->hidden ? compass(x->p.rise_az) : _(compass(x->p.rise_az)),
                         (int)(x->p.top_el * 57.29578f + 0.5f));
                aos_hal_log("cielo", "alert: %s", a->alert_txt);
                aos_ui_toast(a->alert_txt, 6000);
                aos_ui_sound(AOS_SND_NOTIF);
            }
        }
        aos_hal_mutex_unlock(a->mutex);
    }

    /* the files for the page */
    if (!a->hidden && (a->json_ms == 0 || now_ms - a->json_ms > 30000) && !a->pass_busy) {
        a->json_ms = now_ms ? now_ms : 1;
        write_passes(a);
        cl_sat_write_now(a, jd_now);
    }
}
