/*
 * CIELO - tonight's sky from where the board is: the stars, the
 * constellations, the Milky Way, the planets, the Sun and the Moon, the
 * Messier objects, and the satellites with their passes.
 *
 * Life cycle, the frames, the time and the touch. What a frame holds and
 * how it is drawn is cl_view.c; the engine is cl_astro.c and cl_sgp4.c;
 * the rest is listed in cielo.h.
 *
 * The time travel: the sky shows the clock plus an offset, which runs at a
 * rate (paused, real, 60, 600 or 3600 times, forwards or backwards); the
 * arrows move it an hour (a day held down), the date opens a panel, and
 * "Ahora" comes back.
 *
 * Development switches for the simulator (getenv() is NULL on the board):
 * CIELO_JD=<julian date> starts at that moment, CIELO_SCREEN=settings|
 * place|date|sats opens that panel, CIELO_PICK=<body number> picks a body (h<HR number>, a star),
 * CIELO_VIEW="az alt fov" (degrees) looks there, CIELO_ALERT_MIN=<minutes>
 * alerts that long before a pass (to see an alert without waiting for one).
 */
#include "cielo.h"
#include "cl_dmath.h"
#include "cl_mem.h"

#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK    (24 * 1024)
#define IDLE_FRAME_MS   1000
#define FAST_FRAME_MS   33
#define SAT_FRAME_MS    100

static const double RATES[] = { -3600, -600, -60, 0, 1, 60, 600, 3600 };
#define RATE_LIVE 4
#define NRATES ((int)(sizeof RATES / sizeof RATES[0]))

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

double cl_real_jd(void)
{
    return cl_jd_from_unix((double)time(NULL));
}

double cl_now_jd(const app_t *a)
{
    return cl_real_jd() + a->offset_s / 86400.0;
}

void cl_set_time(app_t *a, double jd)
{
    a->offset_s = (jd - cl_real_jd()) * 86400.0;
    cl_dirty(a);
}

void cl_go_live(app_t *a)
{
    a->offset_s = 0;
    a->rate_i = RATE_LIVE;
    a->rate = 1;
    cl_dirty(a);
}

void cl_fmt_time(double jd, char *out, size_t n, int style)
{
    static const char *const WD[7] = { N_("dom"), N_("lun"), N_("mar"), N_("mié"), N_("jue"), N_("vie"), N_("sáb") };
    time_t t = (time_t)cl_unix_from_jd(jd);
    struct tm tm;
    localtime_r(&t, &tm);
    if (style == 1)
        snprintf(out, n, "%s %d/%d/%d %02d:%02d", _(WD[tm.tm_wday % 7]), tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900,
                 tm.tm_hour, tm.tm_min);
    else if (style == 2)
        snprintf(out, n, "%s %d/%d %02d:%02d", _(WD[tm.tm_wday % 7]), tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    else
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void cl_dirty(app_t *a)
{
    a->moved_ms = (uint32_t)aos_hal_uptime_ms();
    a->last_push_ms = 0;
}

/* ---------------------------------------------------------------------------
 * The card of what is picked
 * ------------------------------------------------------------------------- */

static const char *compass16(float az_deg)
{
    static const char *const P[8] = { N_("N"), N_("NE"), N_("E"), N_("SE"), N_("S"), N_("SO"), N_("O"), N_("NO") };
    return _(P[(int)floorf(az_deg / 45.0f + 0.5f) & 7]);
}

static void fmt_dist_ly(char *o, size_t n, double ly)
{
    if (ly >= 1e6) snprintf(o, n, "%.1f %s", ly / 1e6, _("M a.l."));
    else if (ly >= 1e4) snprintf(o, n, "%.0f %s", ly / 1e3, _("mil a.l."));
    else snprintf(o, n, "%.0f %s", ly, _("a.l."));
}

static void fmt_radec(char *o, size_t n, double ra_rad, double dec_rad)
{
    double h = cl_fmod_pos(ra_rad, CL_TWOPI) * 12 / CL_PI;
    int hh = (int)h, mm = (int)((h - hh) * 60);
    double d = dec_rad * CL_R2D;
    char s = d < 0 ? '-' : '+';
    d = cl_dabs(d);
    int dd = (int)d, dm = (int)((d - dd) * 60);
    snprintf(o, n, "%02dh%02dm %c%02d°%02d'", hh, mm, s, dd, dm);
}

static void row(cl_scene_t *s, const char *k, const char *v)
{
    if (s->card_nrows >= 6) return;
    snprintf(s->card_rows[s->card_nrows][0], sizeof s->card_rows[0][0], "%s", k);
    snprintf(s->card_rows[s->card_nrows][1], sizeof s->card_rows[0][1], "%s", v);
    s->card_nrows++;
}

static void rts_foot(app_t *a, cl_scene_t *s, cl_alt_fn fn, void *user, double h0)
{
    cl_rts_t r;
    double jd = cl_now_jd(a);
    cl_rts(fn, user, jd, h0, &r);
    char t1[16], t2[16], t3[16];
    if (r.always_up) {
        cl_fmt_time(r.transit, t2, sizeof t2, 0);
        snprintf(s->card_foot, sizeof s->card_foot, "%s · %s %s", _("Siempre sobre el horizonte"), _("Culmina"), t2);
    } else if (r.never_up) {
        snprintf(s->card_foot, sizeof s->card_foot, "%s", _("No sale en las próximas 24 h"));
    } else {
        cl_fmt_time(r.rise, t1, sizeof t1, 0);
        cl_fmt_time(r.transit, t2, sizeof t2, 0);
        cl_fmt_time(r.set, t3, sizeof t3, 0);
        snprintf(s->card_foot, sizeof s->card_foot, "%s %s  ·  %s %s  ·  %s %s",
                 r.rise < jd ? _("Salió") : _("Sale"), r.rise ? t1 : "-", _("Culmina"), r.transit ? t2 : "-",
                 _("Se pone"), r.set ? t3 : "-");
    }
}

static void altaz_rows(cl_scene_t *s, const float enu[3])
{
    float alt, az;
    cl_enu_altaz(enu, &alt, &az);
    char v[32];
    snprintf(v, sizeof v, "%.1f°", (double)(alt * 57.29578f));
    row(s, _("Altura"), v);
    snprintf(v, sizeof v, "%.0f° %s", (double)(az * 57.29578f), compass16(az * 57.29578f));
    row(s, _("Acimut"), v);
}

static void card_build(app_t *a, cl_scene_t *s, bool with_times)
{
    s->card = a->sel_type != OBJ_NONE;
    if (!s->card) return;
    char foot[128];
    snprintf(foot, sizeof foot, "%s", s->card_foot);
    s->card_nrows = 0;
    s->card_sub[0] = 0;
    double jd = cl_now_jd(a);
    cl_sky_t sky;
    cl_sky_set(&sky, jd, a->conf.lat, a->conf.lon);
    char v[48];
    if (a->sel_type == OBJ_STAR && a->have_cat && a->sel_idx < a->cat.n) {
        int i = a->sel_idx;
        const cl_cat_t *c = &a->cat;
        const char *nm = cl_cat_star_name(c, i);
        const char *d = cl_cat_str(c, c->desig[i]);
        char cons[4];
        cl_cat_star_cons(c, i, cons);
        if (nm) snprintf(s->card_title, sizeof s->card_title, "%s", cl_star_name(nm));
        else {
            /* the designation's first part as the title */
            const char *e = strstr(d, " \xC2\xB7");
            int len = e ? (int)(e - d) : (int)strlen(d);
            snprintf(s->card_title, sizeof s->card_title, "%.*s", len, d);
        }
        snprintf(s->card_sub, sizeof s->card_sub, "%s · %s · %s", _("Estrella"), cons[0] ? cl_cons_name(cons) : "-", d);
        snprintf(v, sizeof v, "%.2f", (double)c->mag[i]);
        row(s, _("Magnitud"), v);
        if (c->plx[i] >= 20) fmt_dist_ly(v, sizeof v, 3.26156 / (c->plx[i] / 10000.0));
        else snprintf(v, sizeof v, "?");
        row(s, _("Distancia"), v);
        snprintf(v, sizeof v, "%s %c", _("Clase"), c->spec[i] ? c->spec[i] : '?');
        row(s, _("Espectro"), v);
        float enu[3];
        for (int k = 0; k < 3; k++)
            enu[k] = sky.m_j2000_enu[k][0] * c->xyz[3 * i] + sky.m_j2000_enu[k][1] * c->xyz[3 * i + 1] +
                     sky.m_j2000_enu[k][2] * c->xyz[3 * i + 2];
        altaz_rows(s, enu);
        fmt_radec(v, sizeof v, atan2f(c->xyz[3 * i + 1], c->xyz[3 * i]), asinf(c->xyz[3 * i + 2]));
        row(s, _("AR / Dec (J2000)"), v);
        if (with_times) {
            cl_rts_star_t u;
            memcpy(u.v, &c->xyz[3 * i], sizeof u.v);
            u.lat = a->conf.lat;
            u.lon = a->conf.lon;
            rts_foot(a, s, cl_alt_star, &u, -0.5667 * CL_D2R);
        }
    } else if (a->sel_type == OBJ_BODY) {
        int b = a->sel_idx;
        cl_body_t bp;
        cl_body(b, jd, &bp);
        snprintf(s->card_title, sizeof s->card_title, "%s", cl_body_name(b));
        snprintf(s->card_sub, sizeof s->card_sub, "%s",
                 b == CL_SUN ? _("Nuestra estrella") : b == CL_MOON ? _("Satélite de la Tierra") : _("Planeta"));
        snprintf(v, sizeof v, "%.1f", (double)bp.mag);
        row(s, _("Magnitud"), v);
        if (b == CL_MOON) snprintf(v, sizeof v, "%.0f km", bp.dist_au * 149597870.7);
        else if (b == CL_SUN) snprintf(v, sizeof v, "%.4f %s", bp.dist_au, _("UA"));
        else snprintf(v, sizeof v, "%.3f %s", bp.dist_au, _("UA"));
        row(s, _("Distancia"), v);
        if (b == CL_SUN) {
            snprintf(v, sizeof v, "%.1f'", (double)(bp.semidiam * 2 * 57.29578f * 60));
            row(s, _("Diámetro"), v);
        } else {
            snprintf(v, sizeof v, "%.0f%%", (double)(bp.phase * 100));
            row(s, b == CL_MOON ? _("Iluminada") : _("Fase"), v);
        }
        float enu[3];
        cl_radec_to_enu(&sky, bp.ra, bp.dec, b == CL_MOON ? bp.dist_au : 0, enu);
        altaz_rows(s, enu);
        if (b == CL_MOON) {
            /* the age: how far the Moon is from the Sun along the ecliptic */
            cl_body_t sp;
            cl_body(CL_SUN, jd, &sp);
            double el = cl_fmod_pos(bp.elon - sp.elon, CL_TWOPI) * CL_R2D;
            const char *ph = el < 12 || el > 348 ? _("Luna nueva")
                           : el < 80 ? _("Creciente") : el < 100 ? _("Cuarto creciente")
                           : el < 168 ? _("Gibosa creciente") : el < 192 ? _("Luna llena")
                           : el < 260 ? _("Gibosa menguante") : el < 280 ? _("Cuarto menguante") : _("Menguante");
            row(s, _("Fase"), ph);
        } else {
            fmt_radec(v, sizeof v, bp.ra, bp.dec);
            row(s, _("AR / Dec"), v);
        }
        if (with_times) {
            cl_rts_body_t u = { b, a->conf.lat, a->conf.lon };
            rts_foot(a, s, cl_alt_body, &u, (b == CL_SUN || b == CL_MOON ? -0.8333 : -0.5667) * CL_D2R);
        }
    } else if (a->sel_type == OBJ_MESS && a->have_cat && a->sel_idx < a->cat.nmess) {
        const struct cl_mess *m = &a->cat.mess[a->sel_idx];
        snprintf(s->card_title, sizeof s->card_title, "M %d", m->m);
        const char *nm = cl_mess_name(m->m), *ngc = cl_cat_str(&a->cat, m->ngc);
        if (nm) snprintf(s->card_sub, sizeof s->card_sub, "%s · %s%s%s", nm, cl_mess_type(m->type), ngc[0] ? " · " : "", ngc);
        else snprintf(s->card_sub, sizeof s->card_sub, "%s%s%s", cl_mess_type(m->type), ngc[0] ? " · " : "", ngc);
        if (m->mag < 90) snprintf(v, sizeof v, "%.1f", (double)m->mag);
        else snprintf(v, sizeof v, "?");
        row(s, _("Magnitud"), v);
        float ly = cl_mess_dist_ly(m->m);
        if (ly > 0) {
            char d[32];
            fmt_dist_ly(d, sizeof d, ly);
            snprintf(v, sizeof v, "≈ %s", d);
        } else {
            snprintf(v, sizeof v, "?");
        }
        row(s, _("Distancia"), v);
        snprintf(v, sizeof v, "%.0f'", (double)m->size);
        row(s, _("Tamaño"), v);
        float enu[3];
        for (int k = 0; k < 3; k++)
            enu[k] = sky.m_j2000_enu[k][0] * m->xyz[0] + sky.m_j2000_enu[k][1] * m->xyz[1] + sky.m_j2000_enu[k][2] * m->xyz[2];
        altaz_rows(s, enu);
        fmt_radec(v, sizeof v, atan2f(m->xyz[1], m->xyz[0]), asinf(m->xyz[2]));
        row(s, _("AR / Dec (J2000)"), v);
        if (with_times) {
            cl_rts_star_t u;
            memcpy(u.v, m->xyz, sizeof u.v);
            u.lat = a->conf.lat;
            u.lon = a->conf.lon;
            rts_foot(a, s, cl_alt_star, &u, -0.5667 * CL_D2R);
        }
    } else if (a->sel_type == OBJ_SAT) {
        aos_hal_mutex_lock(a->mutex);
        int k = a->sel_idx;
        if (k >= a->nsats) {
            aos_hal_mutex_unlock(a->mutex);
            s->card = false;
            return;
        }
        cl_satx_t sx = a->sats[k];
        /* the next visible pass from the shown time */
        cl_pass_t next;
        bool have = false;
        for (int i = 0; i < a->npasses; i++) {
            if (a->passes[i].sat != k || a->passes[i].p.set < jd) continue;
            if (!have || (a->passes[i].p.vis_start && !next.vis_start)) {
                next = a->passes[i].p;
                have = true;
                if (next.vis_start) break;
            }
        }
        aos_hal_mutex_unlock(a->mutex);
        snprintf(s->card_title, sizeof s->card_title, "%s", sx.el.name);
        snprintf(s->card_sub, sizeof s->card_sub, "%s · NORAD %u", _("Satélite"), (unsigned)sx.el.norad);
        cl_look_t lk;
        if (cl_sat_look(&sx.el, jd, a->conf.lat, a->conf.lon, a->conf.height / 1000.0, sx.std_mag, &lk)) {
            float ce = cosf((float)lk.el);
            float e[3] = { ce * sinf((float)lk.az), ce * cosf((float)lk.az), sinf((float)lk.el) };
            altaz_rows(s, e);
            snprintf(v, sizeof v, "%.0f km", lk.range);
            row(s, _("Distancia"), v);
            snprintf(v, sizeof v, "%.0f km", lk.alt_km);
            row(s, _("Altitud"), v);
            row(s, _("Iluminado"), lk.sunlit ? _("sí") : _("no, en sombra"));
            if (lk.sunlit && lk.el > 0) snprintf(v, sizeof v, "%.1f", (double)lk.mag);
            else snprintf(v, sizeof v, "-");
            row(s, _("Magnitud"), v);
        }
        if (have) {
            char t1[32], t2[16];
            cl_fmt_time(next.vis_start ? next.vis_start : next.rise, t1, sizeof t1, 2);
            cl_fmt_time(next.top, t2, sizeof t2, 0);
            snprintf(s->card_foot, sizeof s->card_foot, "%s %s · %s %s, %.0f° %s",
                     next.vis_start ? _("Pasada visible") : _("Pasada (no visible)"), t1, _("máx."), t2,
                     (double)(next.top_el * 57.29578f), compass16(next.top_az * 57.29578f));
        } else {
            snprintf(s->card_foot, sizeof s->card_foot, "%s",
                     a->pass_busy ? _("Calculando pasadas...") : _("Sin pasadas sobre 10° en 3 días"));
        }
        return;
    }
    if (!with_times) snprintf(s->card_foot, sizeof s->card_foot, "%s", foot);
}

void cl_select(app_t *a, int type, int idx)
{
    a->sel_type = type;
    a->sel_idx = idx;
    a->sel_card_ms = 0;
    a->sel_rts_jd = 0;
    a->scene.card_foot[0] = 0;
    cl_dirty(a);
}

/* ---------------------------------------------------------------------------
 * The scene and the frames
 * ------------------------------------------------------------------------- */

static void rate_text(app_t *a, char *o, size_t n)
{
    double r = RATES[a->rate_i];
    if (r == 1) o[0] = 0;
    else if (r == 0) snprintf(o, n, "%s", _("pausa"));
    else snprintf(o, n, "%sx%.0f", r < 0 ? "-" : "", r < 0 ? -r : r);
}

static void scene_build(app_t *a, uint32_t now)
{
    cl_scene_t *s = &a->scene;
    double jd = cl_now_jd(a);
    s->sw = a->sw;
    s->sh = a->sh;
    s->az = a->az;
    s->alt = a->alt;
    s->fov = a->fov;
    s->jd = jd;
    s->lat = a->conf.lat;
    s->lon = a->conf.lon;
    s->night = a->conf.night;
    s->lines = a->conf.lines;
    s->cnames = a->conf.cnames;
    s->snames = a->conf.snames;
    s->mw = a->conf.mw;
    s->messier = a->conf.messier;
    s->grid = a->conf.grid;
    s->ground = a->conf.ground;
    s->atmos = a->conf.atmos;
    s->sats = a->conf.sats;
    s->planets = a->conf.planets;
    s->mag_extra = a->conf.mag_extra;
    s->moving = a->touching || a->pinching || fabsf(a->vaz) > 0.01f || fabsf(a->valt) > 0.01f || a->anim;
    s->sel_type = a->sel_type;
    s->sel_idx = a->sel_idx;
    snprintf(s->top1, sizeof s->top1, "%s", a->conf.place);
    s->live = a->rate_i == RATE_LIVE && cl_dabs(a->offset_s) < 30;
    char t[40];
    cl_fmt_time(jd, t, sizeof t, 1);
    snprintf(s->top2, sizeof s->top2, "%s", t);
    rate_text(a, s->rate, sizeof s->rate);
    s->labels[8] = s->live ? _("Ahora") : _("Volver a ahora");
    /* the hint */
    s->hint[0] = 0;
    if (!a->have_cat) snprintf(s->hint, sizeof s->hint, "%s", _("Falta el pack del cielo (cielo_p4.pak)"));
    else if (!aos_hal_time_is_valid()) snprintf(s->hint, sizeof s->hint, "%s", _("La placa todavía no tiene hora"));

    /* the card: the times when picked, the rest once a second */
    if (a->sel_type != OBJ_NONE) {
        bool times = a->sel_rts_jd == 0 || cl_dabs(jd - a->sel_rts_jd) > 0.25;
        if (times || now - a->sel_card_ms > 1000 || s->moving) {
            if (times || now - a->sel_card_ms > 1000) {
                card_build(a, s, times);
                if (times) a->sel_rts_jd = jd;
                a->sel_card_ms = now;
            }
        }
    } else {
        s->card = false;
    }

    /* the satellites */
    aos_hal_mutex_lock(a->mutex);
    s->nsat = a->nsats;
    memcpy(s->sat, a->sats, sizeof(cl_satx_t) * (size_t)a->nsats);
    s->gen = a->sats_gen;
    s->track_sat = -1;
    if (a->sel_type == OBJ_SAT && a->sel_idx < a->nsats) {
        /* the pass under way, or the next one */
        for (int i = 0; i < a->npasses; i++) {
            const cl_passx_t *x = &a->passes[i];
            if (x->sat != a->sel_idx || x->p.set < jd) continue;
            s->track_sat = a->sel_idx;
            s->track_t0 = x->p.rise;
            s->track_t1 = x->p.set;
            break;
        }
        if (s->track_sat < 0) {
            s->track_sat = a->sel_idx;
            s->track_t0 = jd - 5.0 / 1440;
            s->track_t1 = jd + 5.0 / 1440;
        }
    }
    aos_hal_mutex_unlock(a->mutex);
}

static void push(app_t *a)
{
    aos_hal_mutex_lock(a->mutex);
    a->wscene = a->scene;
    a->req_seq++;
    aos_hal_mutex_unlock(a->mutex);
}

typedef struct {
    app_t *a;
    cl_fb_t fb;
    bool by_y;
} job_t;

static void job_half(void *arg, int part)
{
    job_t *j = (job_t *)arg;
    cl_fb_t fb = j->fb;
    if (j->by_y) {
        int mid = fb.h / 2;
        cl_fb_clip(&fb, 0, part ? mid : 0, fb.w, part ? fb.h : mid);
    } else {
        int mid = fb.w / 2;
        cl_fb_clip(&fb, part ? mid : 0, 0, part ? fb.w : mid, fb.h);
    }
    cl_render(j->a, &j->a->rscene, &fb, part, 2, NULL, NULL);
}

static void worker(void *arg)
{
    app_t *a = (app_t *)arg;
    while (!aos_hal_worker_should_stop()) {
        if (a->req_seq == a->done_seq || !a->viewing || a->hidden || a->over_bits ||
            (!a->use_flip && a->ready >= 0)) {
            aos_hal_worker_sleep(4);
            continue;
        }
        aos_hal_mutex_lock(a->mutex);
        a->rscene = a->wscene;
        uint32_t seq = a->req_seq;
        aos_hal_mutex_unlock(a->mutex);
        const cl_scene_t *s = &a->rscene;
        if (s->sw != a->sw || s->sh != a->sh) {
            a->done_seq = seq;
            continue;
        }
        uint64_t t0 = aos_hal_uptime_us();
        int nh = 0;
        cl_frame_prepare(a, s, a->whits, &nh);

        job_t j;
        j.a = a;
        uint16_t *back = NULL;
        int idx = -1;
        if (a->use_flip) back = aos_hal_display_back();
        if (back) {
            cl_fb_panel(&j.fb, back, AOS_PANEL_W, AOS_PANEL_H, aos_hal_display_get_rotation());
            if (j.fb.w != s->sw || j.fb.h != s->sh) {
                /* the screen turned under us: the next tick lays it out again */
                a->done_seq = seq;
                continue;
            }
        } else {
            if (!a->frame[0]) {
                a->done_seq = seq;
                aos_hal_worker_sleep(20);
                continue;
            }
            idx = a->shown == 0 ? 1 : 0;
            cl_fb_plain(&j.fb, a->frame[idx], s->sw, s->sh);
        }
        j.by_y = j.fb.sx == 1 || j.fb.sx == -1;
        if (!aos_hal_worker_split(job_half, &j)) {
            job_half(&j, 0);
            job_half(&j, 1);
        }
        if (back) {
            if (!a->over_bits && a->viewing) aos_hal_display_flip(back);
        } else {
            a->ready = idx;
        }
        aos_hal_mutex_lock(a->mutex);
        a->wnhits = nh;
        a->hits_new = true;
        aos_hal_mutex_unlock(a->mutex);
        uint32_t us = (uint32_t)(aos_hal_uptime_us() - t0);
        a->draw_us_sum += us;
        if (us > a->draw_us_max) a->draw_us_max = us;
        a->fps_frames++;
        a->done_seq = seq;
    }
}

/* ---------------------------------------------------------------------------
 * The view
 * ------------------------------------------------------------------------- */

#define FOV_MIN (0.5f * 0.017453f)
#define FOV_MAX (200.0f * 0.017453f)
#define ALT_MIN (-20.0f * 0.017453f)
#define ALT_MAX (90.0f * 0.017453f)

static float S_of(const app_t *a)
{
    int m = a->sw < a->sh ? a->sw : a->sh;
    return (float)m * 0.5f / tanf(a->fov * 0.25f);
}

static void view_clamp(app_t *a)
{
    if (a->fov < FOV_MIN) a->fov = FOV_MIN;
    if (a->fov > FOV_MAX) a->fov = FOV_MAX;
    if (a->alt < ALT_MIN) a->alt = ALT_MIN;
    if (a->alt > ALT_MAX) a->alt = ALT_MAX;
    a->az = fmodf(a->az, 6.2831853f);
    if (a->az < 0) a->az += 6.2831853f;
}

static bool dome_turn(const app_t *a)
{
    return a->alt > 1.40f && a->fov > 2.6f;
}

static void view_pan(app_t *a, float dx, float dy, float x, float y)
{
    float S = S_of(a);
    if (dome_turn(a)) {
        /* the whole dome: the finger turns it about the zenith */
        float cx = a->sw * 0.5f, cy = a->sh * 0.5f;
        float a0 = atan2f(y - dy - cy, x - dx - cx), a1 = atan2f(y - cy, x - cx);
        float d = a1 - a0;
        if (d > 3.14159f) d -= 6.28318f;
        if (d < -3.14159f) d += 6.28318f;
        a->az += d;
    } else {
        a->az -= 2.0f * dx / S;
        a->alt += 2.0f * dy / S;
    }
    view_clamp(a);
    cl_dirty(a);
}

static void zoom(app_t *a, float k)
{
    a->fov /= k;
    view_clamp(a);
    cl_dirty(a);
}

static void go_to(app_t *a, float az, float alt, float fov)
{
    /* the shortest way round */
    float d = az - a->az;
    while (d > 3.14159f) d -= 6.28318f;
    while (d < -3.14159f) d += 6.28318f;
    a->az_target = a->az + d;
    a->alt_target = alt;
    a->fov_target = fov;
    a->anim = true;
    a->vaz = a->valt = 0;
}

/* the dome with the pole that is up on top: the south one in the south */
float cl_dome_az(const app_t *a)
{
    return a->conf.lat < 0 ? 0.0f : 3.14159f;
}

static void toggle_mode(app_t *a)
{
    if (a->alt > 1.2f && a->fov > 2.0f) {
        go_to(a, cl_dome_az(a) + 3.14159f, 20 * 0.017453f, 100 * 0.017453f);
    } else {
        float fov = a->sw > a->sh ? 200 * 0.017453f : 190 * 0.017453f;
        go_to(a, cl_dome_az(a), ALT_MAX, fov);
    }
}

/* ---------------------------------------------------------------------------
 * The touch
 * ------------------------------------------------------------------------- */

static void take_hits(app_t *a)
{
    aos_hal_mutex_lock(a->mutex);
    if (a->hits_new) {
        memcpy(a->hits, a->whits, sizeof(cl_hit_t) * (size_t)a->wnhits);
        a->nhits = a->wnhits;
        a->hits_new = false;
    }
    aos_hal_mutex_unlock(a->mutex);
}

static int pick(app_t *a, float x, float y, int *type)
{
    take_hits(a);
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < a->nhits; i++) {
        const cl_hit_t *h = &a->hits[i];
        float dx = (float)h->x - x, dy = (float)h->y - y, d = sqrtf(dx * dx + dy * dy);
        float reach = (float)h->r + 26;
        if (d > reach) continue;
        /* the bodies and satellites win over a star next to them */
        float score = d - (h->type == OBJ_BODY || h->type == OBJ_SAT ? 24 : h->type == OBJ_MESS ? 6 : 0) - h->r * 0.5f;
        if (score < bd) {
            bd = score;
            best = i;
        }
    }
    if (best < 0) return -1;
    *type = a->hits[best].type;
    return a->hits[best].idx;
}

static void time_step(app_t *a, double sec)
{
    a->offset_s += sec;
    cl_dirty(a);
}

static bool buttons(app_t *a, float x, float y)
{
    const cl_lay_t *L = &a->L;
    if (a->sel_type != OBJ_NONE && cl_hit_card(a, x, y)) {
        if (cl_hit_disc(&L->card_close, x, y)) cl_select(a, OBJ_NONE, -1);
        return true;
    }
    if (cl_hit_disc(&L->set, x, y)) {
        cl_show_settings(a);
        return true;
    }
    if (cl_hit_disc(&L->sats, x, y)) {
        cl_show_sats(a);
        return true;
    }
    if (cl_hit_disc(&L->mode, x, y)) {
        toggle_mode(a);
        return true;
    }
    if (cl_hit_disc(&L->night, x, y)) {
        a->conf.night = !a->conf.night;
        cl_conf_save(a);
        cl_dirty(a);
        return true;
    }
    if (x >= L->bar_x && x < L->bar_x + L->bar_w && y >= L->bar_y && y < L->bar_y + L->bar_h) {
        cl_show_date(a);
        return true;
    }
    if (cl_hit_disc(&L->t_now, x, y)) {
        cl_go_live(a);
        return true;
    }
    if (cl_hit_disc(&L->t_back, x, y)) {
        time_step(a, -3600);
        return true;
    }
    if (cl_hit_disc(&L->t_fwd, x, y)) {
        time_step(a, 3600);
        return true;
    }
    if (cl_hit_disc(&L->t_back2, x, y) || cl_hit_disc(&L->t_fwd2, x, y)) {
        int d = cl_hit_disc(&L->t_back2, x, y) ? -1 : 1;
        a->rate_i += d;
        if (a->rate_i < 0) a->rate_i = 0;
        if (a->rate_i >= NRATES) a->rate_i = NRATES - 1;
        a->rate = RATES[a->rate_i];
        cl_dirty(a);
        return true;
    }
    return false;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (a->screen != SCR_SKY) return;
    switch (ev->type) {
    case AOS_GESTURE_DRAG_BEGIN:
        a->touching = true;
        a->anim = false;
        a->vaz = a->valt = 0;
        break;
    case AOS_GESTURE_DRAG:
        view_pan(a, ev->dx, ev->dy, ev->x, ev->y);
        break;
    case AOS_GESTURE_DRAG_END: {
        a->touching = false;
        float S = S_of(a);
        if (!dome_turn(a)) {
            a->vaz = -2.0f * ev->vx / S;
            a->valt = 2.0f * ev->vy / S;
        }
        break;
    }
    case AOS_GESTURE_PINCH_BEGIN:
        a->pinching = true;
        a->anim = false;
        a->vaz = a->valt = 0;
        break;
    case AOS_GESTURE_PINCH:
        if (ev->scale > 0) zoom(a, ev->scale);
        break;
    case AOS_GESTURE_PINCH_END:
        a->pinching = false;
        cl_dirty(a);
        break;
    case AOS_GESTURE_TAP: {
        if (buttons(a, ev->x, ev->y)) break;
        int type = OBJ_NONE, i = pick(a, ev->x, ev->y, &type);
        if (i >= 0) cl_select(a, type, i);
        else if (a->sel_type != OBJ_NONE) cl_select(a, OBJ_NONE, -1);
        break;
    }
    case AOS_GESTURE_DOUBLE_TAP: {
        const cl_lay_t *L = &a->L;
        if (cl_hit_disc(&L->t_back2, ev->x, ev->y) || cl_hit_disc(&L->t_fwd2, ev->x, ev->y) ||
            cl_hit_disc(&L->t_back, ev->x, ev->y) || cl_hit_disc(&L->t_fwd, ev->x, ev->y)) {
            /* a fast tap on the arrows counts twice */
            buttons(a, ev->x, ev->y);
            break;
        }
        if (cl_hit_card(a, ev->x, ev->y) || ev->y < L->bar_y + L->bar_h + 10) break;
        float enu[3], alt, az;
        cl_unproject(&a->scene, ev->x, ev->y, enu);
        cl_enu_altaz(enu, &alt, &az);
        go_to(a, az, alt, a->fov * 0.5f);
        break;
    }
    case AOS_GESTURE_LONG_PRESS:
        if (cl_hit_disc(&a->L.t_back, ev->x, ev->y)) time_step(a, -86400);
        else if (cl_hit_disc(&a->L.t_fwd, ev->x, ev->y)) time_step(a, 86400);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * The timer
 * ------------------------------------------------------------------------- */

static void log_fps(app_t *a, uint32_t now)
{
    if (now - a->fps_t0 < 5000) return;
    uint32_t n = a->fps_frames, dt = now - a->fps_t0;
    if (n) {
        uint32_t li = 0, lp = 0;
        aos_hal_heap_info(&li, &lp);
        aos_hal_log("cielo", "%u.%u fps, a frame %u.%u ms (max %u.%u), %u stars in the pack, PSRAM free %u KB",
                    (unsigned)(n * 1000 / dt), (unsigned)(n * 10000 / dt % 10), (unsigned)(a->draw_us_sum / n / 1000),
                    (unsigned)(a->draw_us_sum / n / 100 % 10), (unsigned)(a->draw_us_max / 1000),
                    (unsigned)(a->draw_us_max / 100 % 10), (unsigned)a->cat.n, (unsigned)(lp / 1024));
    }
    a->fps_frames = 0;
    a->draw_us_sum = 0;
    a->draw_us_max = 0;
    a->fps_t0 = now;
}

static void tick(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    double real = cl_real_jd();
    double dt = a->last_real ? (real - a->last_real) * 86400.0 : 0;
    if (dt < 0 || dt > 5) dt = 0;
    a->last_real = real;
    if (a->rate != 1) a->offset_s += (a->rate - 1) * dt;

    static uint32_t last_conf;
    if (now - last_conf > 2000) {
        last_conf = now;
        cl_conf_poll(a);
    }
    cl_sat_tick(a, cl_now_jd(a), now);
    cl_ui_tick(a, now);
    if (a->hidden || a->screen != SCR_SKY) return;

    uint32_t ov = aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST;
    if (ov != a->over_bits) {
        a->over_bits = ov;
        if (!ov) cl_dirty(a);
    }
    if (ov) return;

    /* the simulator's frames go through the canvas */
    if (!a->use_flip && a->ready >= 0) {
        int r = a->ready;
        lv_canvas_set_buffer(a->canvas, a->frame[r], a->sw, a->sh, LV_COLOR_FORMAT_RGB565);
        a->shown = r;
        a->ready = -1;
        lv_obj_invalidate(a->canvas);
    }

    /* motion */
    float dts = CL_TICK_MS / 1000.0f;
    if (a->anim) {
        float k = 1.0f - expf(-dts * 9.0f);
        a->az += (a->az_target - a->az) * k;
        a->alt += (a->alt_target - a->alt) * k;
        a->fov *= powf(a->fov_target / a->fov, k);
        if (fabsf(a->az_target - a->az) < 0.002f && fabsf(a->alt_target - a->alt) < 0.002f &&
            fabsf(a->fov_target / a->fov - 1) < 0.005f) {
            a->az = a->az_target;
            a->alt = a->alt_target;
            a->fov = a->fov_target;
            a->anim = false;
        }
        view_clamp(a);
        cl_dirty(a);
    } else if (!a->touching && (fabsf(a->vaz) > 0.005f || fabsf(a->valt) > 0.005f)) {
        a->az += a->vaz * dts;
        a->alt += a->valt * dts;
        float k = expf(-dts * 4.5f);
        a->vaz *= k;
        a->valt *= k;
        view_clamp(a);
        cl_dirty(a);
    }

    /* how often: while something moves, as fast as the worker draws;
     * otherwise once a second (a degree every four minutes of the sky's
     * turn is a fraction of a pixel) */
    bool fast = now - a->moved_ms < 300 || a->rate != 1;
    /* a satellite up moves up to a degree a second: ten frames a second
     * keep it from jumping without drawing the whole sky at full rate */
    bool sat_up = false;
    for (int i = 0; i < a->nhits && !fast; i++)
        if (a->hits[i].type == OBJ_SAT) sat_up = true;
    uint32_t every = fast ? FAST_FRAME_MS : sat_up ? SAT_FRAME_MS : IDLE_FRAME_MS;
    if (a->req_seq == a->done_seq && (a->last_push_ms == 0 || now - a->last_push_ms >= every)) {
        a->last_push_ms = now;
        scene_build(a, now);
        push(a);
    }
    take_hits(a);
    log_fps(a, now);
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------- */

static void cl_destroy(aos_app_t *self, void *inst);

static void apply_shape(app_t *a)
{
    a->sw = AOS_SCREEN_W;
    a->sh = AOS_SCREEN_H;
    cl_layout(a);
    lv_obj_set_size(a->sky, a->sw, a->sh);
    lv_obj_set_size(a->touch, a->sw, a->sh);
    if (a->canvas && a->frame[0]) {
        a->ready = -1;
        lv_canvas_set_buffer(a->canvas, a->frame[a->shown], a->sw, a->sh, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(a->canvas, a->sw, a->sh);
    }
    cl_dirty(a);
}

static void fonts_bake(app_t *a)
{
    static const uint32_t SYMS[] = { 0xF0493, 0xF0156, 0xF0141, 0xF0142, 0xF0F65, 0xF03E4 };
    cl_font_bake(&a->font[F_S], &aos_inter_20, NULL, NULL, 0);
    cl_font_bake(&a->font[F_M], &aos_inter_24, &aos_sym_28, SYMS, (int)(sizeof SYMS / sizeof SYMS[0]));
    cl_font_bake(&a->font[F_L], &aos_inter_36, NULL, NULL, 0);
}

static void *cl_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)cl_calloc(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->sel_type = OBJ_NONE;
    a->sel_idx = -1;
    a->rate_i = RATE_LIVE;
    a->rate = 1;
    a->ready = -1;
    a->shown = 0;
    a->screen = SCR_SKY;
    a->viewing = true;
    a->mutex = aos_hal_mutex_create();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x02050C), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t li0 = 0, lp0 = 0;
    aos_hal_heap_info(&li0, &lp0);

    cl_names_prepare();
    cl_conf_defaults(&a->conf);
    cl_conf_load(a);

    char path[200];
    const char *dir = aos_hal_path_apps();
    snprintf(path, sizeof path, "%s/cielo_p4.pak", dir ? dir : ".");
    a->have_cat = cl_cat_load(&a->cat, path);
    if (a->have_cat) cl_cat_make_milky_way(&a->cat);
    a->proj = (float *)cl_malloc(sizeof(float) * 3 * (size_t)(a->cat.n ? a->cat.n : 1));
    a->mwgrid = (uint8_t *)cl_malloc((size_t)(AOS_PANEL_H / 8 + 3) * (AOS_PANEL_H / 8 + 3));
    a->hits = (cl_hit_t *)cl_malloc(sizeof(cl_hit_t) * CL_MAX_HITS);
    a->whits = (cl_hit_t *)cl_malloc(sizeof(cl_hit_t) * CL_MAX_HITS);
    if (!a->mutex || !a->proj || !a->mwgrid || !a->hits || !a->whits || !cl_frame_init(a)) {
        cl_destroy(self, a);
        aos_ui_toast(_("Sin memoria"), 2000);
        return NULL;
    }
    fonts_bake(a);

    /* the words the worker writes, translated here */
    static const char *const CARD[8] = { N_("N"), N_("NE"), N_("E"), N_("SE"), N_("S"), N_("SO"), N_("O"), N_("NO") };
    for (int i = 0; i < 8; i++) a->scene.labels[i] = _(CARD[i]);
    a->scene.labels[8] = _("Ahora");
    a->alert_fmt = _("%s: pasada visible a las %s, sale por el %s, sube a %d°");

    a->az = a->conf.az * 0.017453f;
    a->alt = a->conf.alt * 0.017453f;
    a->fov = a->conf.fov * 0.017453f;
    const char *v = getenv("CIELO_VIEW");
    if (v) {
        float az, alt, fov;
        if (sscanf(v, "%f %f %f", &az, &alt, &fov) == 3) {
            a->az = az * 0.017453f;
            a->alt = alt * 0.017453f;
            a->fov = fov * 0.017453f;
        }
    }
    view_clamp(a);
    const char *am = getenv("CIELO_ALERT_MIN");
    if (am) a->conf.alert_min = atoi(am);
    const char *jd = getenv("CIELO_JD");
    if (jd) cl_set_time(a, strtod(jd, NULL));

    a->sky = lv_obj_create(root);
    lv_obj_remove_style_all(a->sky);
    lv_obj_set_pos(a->sky, 0, 0);
    lv_obj_remove_flag(a->sky, LV_OBJ_FLAG_SCROLLABLE);
    a->use_flip = aos_hal_display_back() != NULL;
    if (!a->use_flip) {
        size_t px = (size_t)AOS_PANEL_W * AOS_PANEL_H;
        a->frame[0] = (uint16_t *)cl_malloc(px * 2);
        a->frame[1] = (uint16_t *)cl_malloc(px * 2);
        if (!a->frame[0] || !a->frame[1]) {
            cl_destroy(self, a);
            aos_ui_toast(_("Sin memoria"), 2000);
            return NULL;
        }
        memset(a->frame[0], 0, px * 2);
        a->canvas = lv_canvas_create(a->sky);
        lv_obj_set_pos(a->canvas, 0, 0);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    }
    a->touch = lv_obj_create(a->sky);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_pos(a->touch, 0, 0);
    aos_gesture_attach(a->touch, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, a);
    apply_shape(a);
    self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    if (a->conf.alerts) self->desc.flags |= AOS_APP_FLAG_BACKGROUND;

    cl_sat_init(a);
    if (!aos_hal_worker_start_on("cielo", worker, a, WORKER_STACK, 0, 3)) aos_hal_log("cielo", "no worker");
    a->timer = lv_timer_create(tick, CL_TICK_MS, a);
    a->fps_t0 = (uint32_t)aos_hal_uptime_ms();

    uint32_t li1 = 0, lp1 = 0;
    aos_hal_heap_info(&li1, &lp1);
    aos_hal_log("cielo", "open: %s (%.4f, %.4f), frames %s, PSRAM taken %u KB, internal %d KB", a->conf.place,
                (double)a->conf.lat, (double)a->conf.lon,
                a->use_flip ? "straight into the panel's free buffer, flipped" : "through LVGL's canvas",
                (unsigned)((lp0 - lp1) / 1024), (int)((int32_t)(li0 - li1) / 1024));

    const char *pk = getenv("CIELO_PICK");
    if (pk && pk[0] == 'h') {
        for (int i = 0; i < a->cat.n; i++)
            if (a->cat.hr[i] == atoi(pk + 1)) cl_select(a, OBJ_STAR, i);
    } else if (pk) {
        cl_select(a, OBJ_BODY, atoi(pk));
    }
    const char *scr = getenv("CIELO_SCREEN");
    if (scr && !strcmp(scr, "settings")) cl_show_settings(a);
    else if (scr && !strcmp(scr, "place")) cl_show_place(a);
    else if (scr && !strcmp(scr, "date")) cl_show_date(a);
    else if (scr && !strcmp(scr, "sats")) cl_show_sats(a);
    return a;
}

static void cl_destroy(aos_app_t *self, void *inst)
{
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    aos_hal_worker_stop();
    cl_sat_free(a);
    if (a->have_cat) {
        a->conf.az = a->az * 57.29578f;
        a->conf.alt = a->alt * 57.29578f;
        a->conf.fov = a->fov * 57.29578f;
        cl_conf_save(a);
    }
    if (self && self->root) lv_obj_clean(self->root);
    for (int i = 0; i < F_N; i++) cl_font_free(&a->font[i]);
    cl_cat_free(&a->cat);
    cl_frame_free();
    cl_free(a->proj);
    cl_free(a->mwgrid);
    cl_free(a->hits);
    cl_free(a->whits);
    cl_free(a->frame[0]);
    cl_free(a->frame[1]);
    cl_free(a);
}

static bool cl_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    if (a->screen != SCR_SKY) {
        cl_show_sky(a);
        return true;
    }
    if (a->sel_type != OBJ_NONE) {
        cl_select(a, OBJ_NONE, -1);
        return true;
    }
    return false;
}

static bool cl_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    a->root = root;
    apply_shape(a);
    if (a->screen != SCR_SKY) cl_ui_relayout(a);
    return true;
}

static void cl_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) a->hidden = true;
}

static void cl_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->hidden = false;
    cl_dirty(a);
}

/* A night sky in a circle: a horizon, a crescent Moon and a few stars
 * joined as a constellation. */
static const uint8_t CL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 92, 92, AIC_CIRCLE, AIC_C_LIT(0x0B1630), 255),
    AIC_RECT(AIC_CENTER, 0, 30, 92, 32, 0, AIC_C_LIT(0x14301E), 255),
    AIC_RECT(AIC_CENTER, 18, -18, 26, 26, AIC_CIRCLE, AIC_C_LIT(0xF2EFD8), 255),
    AIC_RECT(AIC_CENTER, 25, -23, 22, 22, AIC_CIRCLE, AIC_C_LIT(0x0B1630), 255),
    AIC_RECT(AIC_CENTER, -26, -10, 6, 6, AIC_CIRCLE, AIC_C_LIT(0xFFFFFF), 255),
    AIC_RECT(AIC_CENTER, -12, -28, 5, 5, AIC_CIRCLE, AIC_C_LIT(0xCFE0FF), 255),
    AIC_RECT(AIC_CENTER, -30, -30, 4, 4, AIC_CIRCLE, AIC_C_LIT(0xFFE6B0), 255),
    AIC_RECT(AIC_CENTER, -6, -2, 4, 4, AIC_CIRCLE, AIC_C_LIT(0xFFFFFF), 255),
    AIC_END
};

static bool cl_init(aos_app_t *app)
{
    app->desc.id       = "aos.cielo";
    app->desc.name     = "Cielo";
    app->desc.icon     = LV_SYMBOL_EYE_OPEN;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, CL_ICON, sizeof CL_ICON);
    app->desc.color_a  = 0x1A2E5C;
    app->desc.color_b  = 0x070D1C;
    app->desc.order    = 166;
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;

    app->create  = cl_create;
    app->destroy = cl_destroy;
    app->back    = cl_back;
    app->resize  = cl_resize;
    app->hide    = cl_hide;
    app->show    = cl_show;
    return true;
}

AOS_APP_ENTRY(cl_init);
