/*
 * CIELO - where things are (see cl_astro.h).
 *
 * Long angles (a mean longitude that has gone round a thousand times since
 * J2000) are summed and reduced in double; once inside a turn they go to
 * float for the sines, which keeps the error far under an arcsecond and
 * costs the board's FPU, not libgcc's soft-float.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cl_astro.h"
#include "cl_dmath.h"

#include <math.h>
#include <string.h>

#define ARCS (CL_D2R / 3600.0)

/* degrees, reduced to [0, 360) in double, then to radians */
static double rad360(double deg)
{
    return cl_fmod_pos(deg, 360.0) * CL_D2R;
}

/* sin and cos of an angle already within a few turns: float is enough */
static inline double fs(double r) { return (double)sinf((float)r); }
static inline double fc(double r) { return (double)cosf((float)r); }

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

double cl_jd_from_unix(double t)
{
    return CL_JD_UNIX + t / 86400.0;
}

double cl_unix_from_jd(double jd)
{
    return (jd - CL_JD_UNIX) * 86400.0;
}

double cl_delta_t(double jd)
{
    double y = 2000.0 + (jd - CL_J2000) / 365.25;
    if (y < 1961) {
        double u = (y - 1820) / 100;
        return -20 + 32 * u * u;
    }
    if (y < 1986) {
        double t = y - 1975;
        return 45.45 + 1.067 * t - t * t / 260 - t * t * t / 718;
    }
    if (y < 2005) {
        double t = y - 2000;
        return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t +
               0.000651814 * t * t * t * t + 0.00002373599 * t * t * t * t * t;
    }
    /* Espenak and Meeus' 62.92 + 0.32217 t + 0.005589 t^2 runs well ahead
     * of what was measured since (69.2 s in 2025, nearly flat); a line
     * through the measured values, and slower after them */
    if (y < 2025) return 64.7 + 0.225 * (y - 2005);
    return 69.2 + 0.1 * (y - 2025);
}

double cl_gmst_deg(double jd)
{
    double d = jd - CL_J2000, t = d / 36525.0;
    double g = 280.46061837 + 360.98564736629 * d + 0.000387933 * t * t - t * t * t / 38710000.0;
    return cl_fmod_pos(g, 360.0);
}

/* ---------------------------------------------------------------------------
 * Nutation (the main four terms) and the obliquity
 * ------------------------------------------------------------------------- */

static void nutation(double jde, double *dpsi, double *deps, double *eps0)
{
    double t = (jde - CL_J2000) / 36525.0;
    double om = rad360(125.04452 - 1934.136261 * t);
    double l = rad360(280.4665 + 36000.7698 * t);
    double lp = rad360(218.3165 + 481267.8813 * t);
    *dpsi = (-17.20 * fs(om) - 1.32 * fs(2 * l) - 0.23 * fs(2 * lp) + 0.21 * fs(2 * om)) * ARCS;
    *deps = (9.20 * fc(om) + 0.57 * fc(2 * l) + 0.10 * fc(2 * lp) - 0.09 * fc(2 * om)) * ARCS;
    *eps0 = (23.4392911 * 3600.0 - 46.8150 * t - 0.00059 * t * t + 0.001813 * t * t * t) * ARCS;
}

/* ---------------------------------------------------------------------------
 * The frame of a moment
 * ------------------------------------------------------------------------- */

static void mat_mul(double r[3][3], const double a[3][3], const double b[3][3])
{
    double t[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(r, t, sizeof t);
}

/* J2000 mean equator -> mean equator of date, Meeus 21.2 */
static void precession(double jde, double p[3][3])
{
    double t = (jde - CL_J2000) / 36525.0;
    double zeta = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t * t * t) * ARCS;
    double z = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t * t * t) * ARCS;
    double th = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t * t * t) * ARCS;
    double cze, sze, cz, sz, cth, sth;
    cl_dsincos(zeta, &sze, &cze);
    cl_dsincos(z, &sz, &cz);
    cl_dsincos(th, &sth, &cth);
    p[0][0] = cze * cth * cz - sze * sz;
    p[0][1] = -sze * cth * cz - cze * sz;
    p[0][2] = -sth * cz;
    p[1][0] = cze * cth * sz + sze * cz;
    p[1][1] = -sze * cth * sz + cze * cz;
    p[1][2] = -sth * sz;
    p[2][0] = cze * sth;
    p[2][1] = -sze * sth;
    p[2][2] = cth;
}

/* mean equator of date -> true equator of date: R1(-eps) R3(-dpsi) R1(eps0) */
static void nutation_matrix(double eps0, double eps, double dpsi, double n[3][3])
{
    double se0, ce0, se, ce, sp, cp;
    cl_dsincos(eps0, &se0, &ce0);
    cl_dsincos(eps, &se, &ce);
    cl_dsincos(dpsi, &sp, &cp);
    n[0][0] = cp;
    n[0][1] = -sp * ce0;
    n[0][2] = -sp * se0;
    n[1][0] = sp * ce;
    n[1][1] = cp * ce * ce0 + se * se0;
    n[1][2] = cp * ce * se0 - se * ce0;
    n[2][0] = sp * se;
    n[2][1] = cp * se * ce0 - ce * se0;
    n[2][2] = cp * se * se0 + ce * ce0;
}

void cl_precess(double jde, const double in[3], double out[3])
{
    double p[3][3];
    precession(jde, p);
    for (int i = 0; i < 3; i++) out[i] = p[i][0] * in[0] + p[i][1] * in[1] + p[i][2] * in[2];
}

void cl_sky_set(cl_sky_t *s, double jd, double lat_deg, double lon_deg)
{
    double jde = jd + cl_delta_t(jd) / 86400.0;
    double dpsi, deps, eps0;
    nutation(jde, &dpsi, &deps, &eps0);
    s->jd = jd;
    s->lat = lat_deg * CL_D2R;
    s->lon = lon_deg * CL_D2R;
    s->eps = eps0 + deps;
    s->dpsi = dpsi;
    /* the apparent sidereal time: the mean one plus the equation of the
     * equinoxes */
    double gast = cl_gmst_deg(jd) * CL_D2R + dpsi * cl_dcos(s->eps);
    s->lst = cl_fmod_pos(gast + s->lon, CL_TWOPI);

    double sl, cl, sp, cp;
    cl_dsincos(s->lst, &sl, &cl);
    cl_dsincos(s->lat, &sp, &cp);
    double h[3][3] = {
        { -sl, cl, 0 },
        { -sp * cl, -sp * sl, cp },
        { cp * cl, cp * sl, sp },
    };
    memcpy(s->m_date_enu_d, h, sizeof h);
    double p[3][3], n[3][3], m[3][3];
    precession(jde, p);
    nutation_matrix(eps0, s->eps, dpsi, n);
    mat_mul(m, n, p);
    mat_mul(m, h, m);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            s->m_j2000_enu[i][j] = (float)m[i][j];
            s->m_date_enu[i][j] = (float)h[i][j];
        }
}

void cl_radec_to_enu(const cl_sky_t *s, double ra, double dec, double dist_au, float enu[3])
{
    double sr, cr, sd, cd;
    cl_dsincos(ra, &sr, &cr);
    cl_dsincos(dec, &sd, &cd);
    double v[3] = { cd * cr, cd * sr, sd };
    if (dist_au > 0) {
        /* topocentric: the observer's place on the geoid, in units of the
         * body's distance (Meeus 11; the height left out) */
        const double re_au = 6378.14 / 149597870.7;
        double f = 1.0 / 298.257, slat, clat;
        cl_dsincos(s->lat, &slat, &clat);
        double u = cl_datan2((1 - f) * slat, clat);
        double su, cu;
        cl_dsincos(u, &su, &cu);
        double rcos = cu, rsin = (1 - f) * su;
        double sl, cl;
        cl_dsincos(s->lst, &sl, &cl);
        double k = re_au / dist_au;
        v[0] -= k * rcos * cl;
        v[1] -= k * rcos * sl;
        v[2] -= k * rsin;
        double n = cl_dsqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
    for (int i = 0; i < 3; i++)
        enu[i] = (float)(s->m_date_enu_d[i][0] * v[0] + s->m_date_enu_d[i][1] * v[1] + s->m_date_enu_d[i][2] * v[2]);
}

void cl_enu_altaz(const float enu[3], float *alt, float *az)
{
    float u = enu[2];
    if (u > 1) u = 1;
    if (u < -1) u = -1;
    *alt = asinf(u);
    float a = atan2f(enu[0], enu[1]);
    if (a < 0) a += 6.2831853f;
    *az = a;
}

void cl_enu_to_j2000(const cl_sky_t *s, const float enu[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = s->m_j2000_enu[0][i] * enu[0] + s->m_j2000_enu[1][i] * enu[1] + s->m_j2000_enu[2][i] * enu[2];
}

float cl_refraction(float alt)
{
    float h = alt * 57.29578f;
    if (h < -1.9f) return 0;
    /* Saemundsson, for the true altitude, in arcminutes */
    float r = 1.02f / tanf((h + 10.3f / (h + 5.11f)) * 0.01745329f);
    if (r < 0) r = 0;
    return r / 60.0f * 0.01745329f;
}

/* ---------------------------------------------------------------------------
 * The Sun (Meeus 25, low precision)
 * ------------------------------------------------------------------------- */

/* true geometric longitude (of date, radians) and distance (AU) */
static void sun_geo(double jde, double *lon, double *r)
{
    double t = (jde - CL_J2000) / 36525.0;
    double l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t;
    double m = rad360(357.52911 + 35999.05029 * t - 0.0001537 * t * t);
    double e = 0.016708634 - 0.000042037 * t - 0.0000001267 * t * t;
    double c = (1.914602 - 0.004817 * t - 0.000014 * t * t) * fs(m) + (0.019993 - 0.000101 * t) * fs(2 * m) +
               0.000289 * fs(3 * m);
    double nu = m + c * CL_D2R;
    *lon = rad360(l0 + c);
    *r = 1.000001018 * (1 - e * e) / (1 + e * fc(nu));
}

/* ---------------------------------------------------------------------------
 * The Moon (Meeus 47)
 * ------------------------------------------------------------------------- */

typedef struct {
    int8_t d, m, mp, f;
    int32_t l;      /* 1e-6 degrees */
    int32_t r;      /* metres */
} moon_lr_t;

static const moon_lr_t MOON_LR[] = {
    { 0, 0, 1, 0, 6288774, -20905355 }, { 2, 0, -1, 0, 1274027, -3699111 },
    { 2, 0, 0, 0, 658314, -2955968 },   { 0, 0, 2, 0, 213618, -569925 },
    { 0, 1, 0, 0, -185116, 48888 },     { 0, 0, 0, 2, -114332, -3149 },
    { 2, 0, -2, 0, 58793, 246158 },     { 2, -1, -1, 0, 57066, -152138 },
    { 2, 0, 1, 0, 53322, -170733 },     { 2, -1, 0, 0, 45758, -204586 },
    { 0, 1, -1, 0, -40923, -129620 },   { 1, 0, 0, 0, -34720, 108743 },
    { 0, 1, 1, 0, -30383, 104755 },     { 2, 0, 0, -2, 15327, 10321 },
    { 0, 0, 1, 2, -12528, 0 },          { 0, 0, 1, -2, 10980, 79661 },
    { 4, 0, -1, 0, 10675, -34782 },     { 0, 0, 3, 0, 10034, -23210 },
    { 4, 0, -2, 0, 8548, -21636 },      { 2, 1, -1, 0, -7888, 24208 },
    { 2, 1, 0, 0, -6766, 30824 },       { 1, 0, -1, 0, -5163, -8379 },
    { 1, 1, 0, 0, 4987, -16675 },       { 2, -1, 1, 0, 4036, -12831 },
    { 2, 0, 2, 0, 3994, -10445 },       { 4, 0, 0, 0, 3861, -11650 },
    { 2, 0, -3, 0, 3665, 14403 },       { 0, 1, -2, 0, -2689, -7003 },
    { 2, 0, -1, 2, -2602, 0 },          { 2, -1, -2, 0, 2390, 10056 },
    { 1, 0, 1, 0, -2348, 6322 },        { 2, -2, 0, 0, 2236, -9884 },
    { 0, 1, 2, 0, -2120, 5751 },        { 0, 2, 0, 0, -2069, 0 },
    { 2, -2, -1, 0, 2048, -4950 },      { 2, 0, 1, -2, -1773, 4130 },
    { 2, 0, 0, 2, -1595, 0 },           { 4, -1, -1, 0, 1215, -3958 },
    { 0, 0, 2, 2, -1110, 0 },           { 3, 0, -1, 0, -892, 3258 },
    { 2, 1, 1, 0, -810, 2616 },         { 4, -1, -2, 0, 759, -1897 },
    { 0, 2, -1, 0, -713, -2117 },       { 2, 2, -1, 0, -700, 2354 },
    { 2, 1, -2, 0, 691, 0 },            { 2, -1, 0, -2, 596, 0 },
    { 4, 0, 1, 0, 549, -1423 },         { 0, 0, 4, 0, 537, -1117 },
    { 4, -1, 0, 0, 520, -1571 },        { 1, 0, -2, 0, -487, -1739 },
    { 2, 1, 0, -2, -399, 0 },           { 0, 0, 2, -2, -381, -4421 },
    { 1, 1, 1, 0, 351, 0 },             { 3, 0, -2, 0, -340, 0 },
    { 4, 0, -3, 0, 330, 0 },            { 2, -1, 2, 0, 327, 0 },
    { 0, 2, 1, 0, -323, 1165 },         { 1, 1, -1, 0, 299, 0 },
    { 2, 0, 3, 0, 294, 0 },             { 2, 0, -1, -2, 0, 8752 },
};

typedef struct {
    int8_t d, m, mp, f;
    int32_t b;      /* 1e-6 degrees */
} moon_b_t;

static const moon_b_t MOON_B[] = {
    { 0, 0, 0, 1, 5128122 }, { 0, 0, 1, 1, 280602 },  { 0, 0, 1, -1, 277693 }, { 2, 0, 0, -1, 173237 },
    { 2, 0, -1, 1, 55413 },  { 2, 0, -1, -1, 46271 }, { 2, 0, 0, 1, 32573 },   { 0, 0, 2, 1, 17198 },
    { 2, 0, 1, -1, 9266 },   { 0, 0, 2, -1, 8822 },   { 2, -1, 0, -1, 8216 },  { 2, 0, -2, -1, 4324 },
    { 2, 0, 1, 1, 4200 },    { 2, 1, 0, -1, -3359 },  { 2, -1, -1, 1, 2463 },  { 2, -1, 0, 1, 2211 },
    { 2, -1, -1, -1, 2065 }, { 0, 1, -1, -1, -1870 }, { 4, 0, -1, -1, 1828 },  { 0, 1, 0, 1, -1794 },
    { 0, 0, 0, 3, -1749 },   { 0, 1, -1, 1, -1565 },  { 1, 0, 0, 1, -1491 },   { 0, 1, 1, 1, -1475 },
    { 0, 1, 1, -1, -1410 },  { 0, 1, 0, -1, -1344 },  { 1, 0, 0, -1, -1335 },  { 0, 0, 3, 1, 1107 },
    { 4, 0, 0, -1, 1021 },   { 4, 0, -1, 1, 833 },    { 0, 0, 1, -3, 777 },    { 4, 0, -2, 1, 671 },
    { 2, 0, 0, -3, 607 },    { 2, 0, 2, -1, 596 },    { 2, -1, 1, -1, 491 },   { 2, 0, -2, 1, -451 },
    { 0, 0, 3, -1, 439 },    { 2, 0, 2, 1, 422 },     { 2, 0, -3, -1, 421 },   { 2, 1, -1, 1, -366 },
    { 2, 1, 0, 1, -351 },    { 4, 0, 0, 1, 331 },     { 2, -1, 1, 1, 315 },    { 2, -2, 0, -1, 302 },
    { 0, 0, 1, 3, -283 },
};

/* ecliptic of date (mean equinox), radians; distance km */
static void moon_geo(double jde, double *lon, double *lat, double *dist)
{
    double t = (jde - CL_J2000) / 36525.0, t2 = t * t, t3 = t2 * t, t4 = t3 * t;
    double lp = 218.3164477 + 481267.88123421 * t - 0.0015786 * t2 + t3 / 538841.0 - t4 / 65194000.0;
    double d = rad360(297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868.0 - t4 / 113065000.0);
    double m = rad360(357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000.0);
    double mp = rad360(134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699.0 - t4 / 14712000.0);
    double f = rad360(93.2720950 + 483202.0175233 * t - 0.0036539 * t2 - t3 / 3526000.0 + t4 / 863310000.0);
    double a1 = rad360(119.75 + 131.849 * t), a2 = rad360(53.09 + 479264.290 * t), a3 = rad360(313.45 + 481266.484 * t);
    double e = 1 - 0.002516 * t - 0.0000074 * t2;
    double lpr = rad360(lp);

    double sl = 0, sr = 0, sb = 0;
    for (unsigned i = 0; i < sizeof MOON_LR / sizeof MOON_LR[0]; i++) {
        const moon_lr_t *k = &MOON_LR[i];
        double arg = k->d * d + k->m * m + k->mp * mp + k->f * f;
        double w = k->m == 0 ? 1 : (k->m == 1 || k->m == -1) ? e : e * e;
        float s, c;
        float af = (float)cl_fmod_pos(arg, CL_TWOPI);
        s = sinf(af);
        c = cosf(af);
        sl += w * k->l * (double)s;
        sr += w * k->r * (double)c;
    }
    for (unsigned i = 0; i < sizeof MOON_B / sizeof MOON_B[0]; i++) {
        const moon_b_t *k = &MOON_B[i];
        double arg = k->d * d + k->m * m + k->mp * mp + k->f * f;
        double w = k->m == 0 ? 1 : (k->m == 1 || k->m == -1) ? e : e * e;
        sb += w * k->b * (double)sinf((float)cl_fmod_pos(arg, CL_TWOPI));
    }
    sl += 3958 * fs(a1) + 1962 * fs(cl_fmod_pos(lpr - f, CL_TWOPI)) + 318 * fs(a2);
    sb += -2235 * fs(lpr) + 382 * fs(a3) + 175 * fs(cl_fmod_pos(a1 - f, CL_TWOPI)) +
          175 * fs(cl_fmod_pos(a1 + f, CL_TWOPI)) + 127 * fs(cl_fmod_pos(lpr - mp, CL_TWOPI)) -
          115 * fs(cl_fmod_pos(lpr + mp, CL_TWOPI));
    *lon = rad360(lp + sl / 1e6);
    *lat = sb / 1e6 * CL_D2R;
    *dist = 385000.56 + sr / 1000.0;
}

/* ---------------------------------------------------------------------------
 * The planets
 * ------------------------------------------------------------------------- */

typedef struct {
    double n0, n1;      /* longitude of the ascending node, degrees, + per day */
    double i0, i1;      /* inclination */
    double w0, w1;      /* argument of perihelion */
    double a0, a1;      /* semi-major axis, AU */
    double e0, e1;      /* eccentricity */
    double m0, m1;      /* mean anomaly */
} elem_t;

/* index CL_MERCURY - 2 ... CL_NEPTUNE - 2; epoch 2000 Jan 0.0 (JD 2451543.5) */
static const elem_t ELEM[7] = {
    { 48.3313, 3.24587e-5, 7.0047, 5.00e-8, 29.1241, 1.01444e-5, 0.387098, 0, 0.205635, 5.59e-10, 168.6562, 4.0923344368 },
    { 76.6799, 2.46590e-5, 3.3946, 2.75e-8, 54.8910, 1.38374e-5, 0.723330, 0, 0.006773, -1.302e-9, 48.0052, 1.6021302244 },
    { 49.5574, 2.11081e-5, 1.8497, -1.78e-8, 286.5016, 2.92961e-5, 1.523688, 0, 0.093405, 2.516e-9, 18.6021, 0.5240207766 },
    { 100.4542, 2.76854e-5, 1.3030, -1.557e-7, 273.8777, 1.64505e-5, 5.20256, 0, 0.048498, 4.469e-9, 19.8950, 0.0830853001 },
    { 113.6634, 2.38980e-5, 2.4886, -1.081e-7, 339.3939, 2.97661e-5, 9.55475, 0, 0.055546, -9.499e-9, 316.9670, 0.0334442282 },
    { 74.0005, 1.3978e-5, 0.7733, 1.9e-8, 96.6612, 3.0565e-5, 19.18171, -1.55e-8, 0.047318, 7.45e-9, 142.5905, 0.011725806 },
    { 131.7806, 3.0173e-5, 1.7700, -2.55e-7, 272.8461, -6.027e-6, 30.05826, 3.313e-8, 0.008606, 2.15e-9, 260.2471, 0.005995147 },
};

static double kepler(double m, double e)
{
    double ea = m + e * fs(m) * (1.0 + e * fc(m));
    for (int i = 0; i < 8; i++) {
        double s, c;
        cl_dsincos(ea, &s, &c);
        double d = (ea - e * s - m) / (1 - e * c);
        ea -= d;
        if (cl_dabs(d) < 1e-12) break;
    }
    return ea;
}

/* heliocentric ecliptic of date, AU, at d days from 2000 Jan 0.0 */
static void planet_helio(int k, double d, double xyz[3])
{
    const elem_t *el = &ELEM[k];
    double n = rad360(el->n0 + el->n1 * d), i = (el->i0 + el->i1 * d) * CL_D2R;
    double w = rad360(el->w0 + el->w1 * d), a = el->a0 + el->a1 * d, e = el->e0 + el->e1 * d;
    double m = rad360(el->m0 + el->m1 * d);
    double ea = kepler(m, e), se, ce;
    cl_dsincos(ea, &se, &ce);
    double xv = a * (ce - e), yv = a * cl_dsqrt(1 - e * e) * se;
    double v = cl_datan2(yv, xv), r = cl_dsqrt(xv * xv + yv * yv);
    double sn, cn, svw, cvw, si, ci;
    cl_dsincos(n, &sn, &cn);
    cl_dsincos(v + w, &svw, &cvw);
    cl_dsincos(i, &si, &ci);
    xyz[0] = r * (cn * cvw - sn * svw * ci);
    xyz[1] = r * (sn * cvw + cn * svw * ci);
    xyz[2] = r * svw * si;

    /* the perturbations of Jupiter, Saturn and Uranus on one another */
    if (k >= 3 && k <= 5) {
        double mj = rad360(ELEM[3].m0 + ELEM[3].m1 * d) * CL_R2D;
        double ms = rad360(ELEM[4].m0 + ELEM[4].m1 * d) * CL_R2D;
        double mu = rad360(ELEM[5].m0 + ELEM[5].m1 * d) * CL_R2D;
#define S(x) fs(rad360(x))
#define C(x) fc(rad360(x))
        double dl = 0, db = 0;
        if (k == 3) {
            dl = -0.332 * S(2 * mj - 5 * ms - 67.6) - 0.056 * S(2 * mj - 2 * ms + 21) + 0.042 * S(3 * mj - 5 * ms + 21) -
                 0.036 * S(mj - 2 * ms) + 0.022 * C(mj - ms) + 0.023 * S(2 * mj - 3 * ms + 52) -
                 0.016 * S(mj - 5 * ms - 69);
        } else if (k == 4) {
            dl = 0.812 * S(2 * mj - 5 * ms - 67.6) - 0.229 * C(2 * mj - 4 * ms - 2) + 0.119 * S(mj - 2 * ms - 3) +
                 0.046 * S(2 * mj - 6 * ms - 69) + 0.014 * S(mj - 3 * ms + 32);
            db = -0.020 * C(2 * mj - 4 * ms - 2) + 0.018 * S(2 * mj - 6 * ms - 49);
        } else {
            dl = 0.040 * S(ms - 2 * mu + 6) + 0.035 * S(ms - 3 * mu + 33) - 0.015 * S(mj - mu + 20);
        }
#undef S
#undef C
        double lon = cl_datan2(xyz[1], xyz[0]);
        double lat = cl_datan2(xyz[2], cl_dsqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1]));
        lon += dl * CL_D2R;
        lat += db * CL_D2R;
        double sl, cl, sb, cb;
        cl_dsincos(lon, &sl, &cl);
        cl_dsincos(lat, &sb, &cb);
        xyz[0] = r * cb * cl;
        xyz[1] = r * cb * sl;
        xyz[2] = r * sb;
    }
}

static float planet_mag(int body, double r, double delta, double i_deg, double sin_b_ring)
{
    float l5 = 5.0f * log10f((float)(r * delta));
    float i = (float)i_deg;
    switch (body) {
    case CL_MERCURY: return -0.42f + l5 + 0.0380f * i - 0.000273f * i * i + 0.000002f * i * i * i;
    case CL_VENUS:   return -4.40f + l5 + 0.0009f * i + 0.000239f * i * i - 0.00000065f * i * i * i;
    case CL_MARS:    return -1.52f + l5 + 0.016f * i;
    case CL_JUPITER: return -9.40f + l5 + 0.005f * i;
    case CL_SATURN: {
        float sb = fabsf((float)sin_b_ring);
        return -8.88f + l5 + 0.044f * i - 2.60f * sb + 1.25f * sb * sb;
    }
    case CL_URANUS:  return -7.19f + l5;
    default:         return -6.87f + l5;
    }
}

/* equatorial semidiameters at 1 AU, arcseconds */
static const float SEMI_1AU[7] = { 3.36f, 8.41f, 4.68f, 98.44f, 82.73f, 35.02f, 33.50f };

static void ecl_to_eq(double lon, double lat, double eps, double *ra, double *dec)
{
    double sl, cl, sb, cb, se, ce;
    cl_dsincos(lon, &sl, &cl);
    cl_dsincos(lat, &sb, &cb);
    cl_dsincos(eps, &se, &ce);
    double x = cb * cl, y = cb * sl * ce - sb * se, z = cb * sl * se + sb * ce;
    *ra = cl_fmod_pos(cl_datan2(y, x), CL_TWOPI);
    *dec = cl_dasin(z);
}

void cl_body(int which, double jd, cl_body_t *o)
{
    memset(o, 0, sizeof *o);
    double jde = jd + cl_delta_t(jd) / 86400.0;
    double dpsi, deps, eps0;
    nutation(jde, &dpsi, &deps, &eps0);
    double eps = eps0 + deps;
    double slon, sr;
    sun_geo(jde, &slon, &sr);

    double lon, lat, dist;
    if (which == CL_SUN) {
        /* aberration (-20.5") on top of the nutation */
        lon = slon + dpsi - 20.4898 / sr * ARCS;
        lat = 0;
        dist = sr;
        o->mag = -26.74f;
        o->phase = 1;
        o->semidiam = (float)(959.63 / sr * ARCS);
    } else if (which == CL_MOON) {
        double km;
        moon_geo(jde, &lon, &lat, &km);
        lon += dpsi;
        dist = km / 149597870.7;
        o->semidiam = (float)(358473400.0 / km * ARCS);
        /* the phase angle from the elongation (Meeus 48.2-48.3) */
        double ce = cl_dcos(lat) * cl_dcos(lon - (slon + dpsi));
        double el = cl_dacos(ce);
        double i = cl_datan2(sr * cl_dsin(el), dist - sr * ce);
        o->phase_angle = (float)i;
        o->phase = (float)((1 + cl_dcos(i)) / 2);
        double id = i * CL_R2D;
        o->mag = (float)(-12.73 + 0.026 * id + 4e-9 * id * id * id * id);
    } else {
        int k = which - CL_MERCURY;
        double d = jde - 2451543.5;
        double es[3] = { sr * cl_dcos(slon), sr * cl_dsin(slon), 0 };   /* the Sun from the Earth */
        double h[3], g[3], delta = 0;
        /* one step of light time is enough for an arcsecond */
        for (int it = 0; it < 2; it++) {
            planet_helio(k, d - delta * 0.0057755183, h);
            g[0] = h[0] + es[0];
            g[1] = h[1] + es[1];
            g[2] = h[2];
            delta = cl_dsqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        }
        double r = cl_dsqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        lon = cl_fmod_pos(cl_datan2(g[1], g[0]), CL_TWOPI) + dpsi;
        lat = cl_datan2(g[2], cl_dsqrt(g[0] * g[0] + g[1] * g[1]));
        dist = delta;
        o->r_au = r;
        double ci = (r * r + delta * delta - sr * sr) / (2 * r * delta);
        double i = cl_dacos(ci);
        o->phase_angle = (float)i;
        o->phase = (float)((1 + ci) / 2);
        double sinb = 0;
        if (which == CL_SATURN) {
            /* the rings' tilt seen from the Earth: their pole is at
             * RA 40.59, Dec 83.54 (J2000), near enough for a magnitude */
            double pra = 40.589 * CL_D2R, pde = 83.537 * CL_D2R, ra, dec;
            ecl_to_eq(lon, lat, eps, &ra, &dec);
            sinb = -(cl_dcos(pde) * cl_dcos(pra) * cl_dcos(dec) * cl_dcos(ra) +
                     cl_dcos(pde) * cl_dsin(pra) * cl_dcos(dec) * cl_dsin(ra) + cl_dsin(pde) * cl_dsin(dec));
        }
        o->mag = planet_mag(which, r, delta, i * CL_R2D, sinb);
        o->semidiam = (float)(SEMI_1AU[k] / delta * ARCS);
    }
    o->elon = cl_fmod_pos(lon, CL_TWOPI);
    o->elat = lat;
    o->dist_au = dist;
    ecl_to_eq(o->elon, lat, eps, &o->ra, &o->dec);
}

/* ---------------------------------------------------------------------------
 * Rise, culmination, set
 * ------------------------------------------------------------------------- */

#define STEP (10.0 / 1440.0)

static double alt_minus(cl_alt_fn fn, void *user, double jd, double h0)
{
    double ha;
    return fn(jd, user, &ha) - h0;
}

/* the crossing of h0 between a and b (f changes sign there) */
static double refine(cl_alt_fn fn, void *user, double a, double fa, double b, double h0)
{
    for (int i = 0; i < 14; i++) {
        double m = 0.5 * (a + b), fm = alt_minus(fn, user, m, h0);
        if ((fm > 0) == (fa > 0)) {
            a = m;
            fa = fm;
        } else {
            b = m;
        }
    }
    return 0.5 * (a + b);
}

/* the first crossing from jd towards jd + dir * span, rising (want_up) or
 * setting; 0 if none */
static double cross(cl_alt_fn fn, void *user, double jd, int dir, double span, double h0, bool want_up)
{
    double t = jd, f = alt_minus(fn, user, t, h0);
    int n = (int)(span / STEP) + 1;
    for (int i = 0; i < n; i++) {
        double t2 = t + dir * STEP, f2 = alt_minus(fn, user, t2, h0);
        /* in time order: (ta, fa) before (tb, fb) */
        double ta = dir > 0 ? t : t2, fa = dir > 0 ? f : f2, tb = dir > 0 ? t2 : t, fb = dir > 0 ? f2 : f;
        bool up = fa <= 0 && fb > 0, down = fa > 0 && fb <= 0;
        if ((want_up && up) || (!want_up && down)) return refine(fn, user, ta, fa, tb, h0);
        t = t2;
        f = f2;
    }
    return 0;
}

/* the upper culmination between a and b: the hour angle going from - to + */
static double culmination(cl_alt_fn fn, void *user, double a, double b, float *alt)
{
    double ha0, best_t = 0, best_alt = -10;
    fn(a, user, &ha0);
    int n = (int)((b - a) / STEP) + 1;
    double t = a;
    for (int i = 0; i < n; i++) {
        double t2 = t + STEP > b ? b : t + STEP, ha1;
        double al = fn(t2, user, &ha1);
        if (al > best_alt) {
            best_alt = al;
            best_t = t2;
        }
        if (ha0 < 0 && ha1 >= 0 && ha1 - ha0 < CL_PI) {
            double lo = t, hi = t2;
            for (int k = 0; k < 14; k++) {
                double m = 0.5 * (lo + hi), ham;
                fn(m, user, &ham);
                if (ham < 0) lo = m;
                else hi = m;
            }
            double tc = 0.5 * (lo + hi), hac;
            *alt = (float)fn(tc, user, &hac);
            return tc;
        }
        ha0 = ha1;
        t = t2;
        if (t >= b) break;
    }
    *alt = (float)best_alt;
    return best_t;
}

void cl_rts(cl_alt_fn fn, void *user, double jd, double h0, cl_rts_t *o)
{
    memset(o, 0, sizeof *o);
    bool up = alt_minus(fn, user, jd, h0) > 0;
    if (up) {
        o->rise = cross(fn, user, jd, -1, 1.05, h0, true);
        o->set = cross(fn, user, jd, 1, 1.05, h0, false);
        if (!o->rise && !o->set) {
            o->always_up = true;
            o->transit = culmination(fn, user, jd - 0.5, jd + 0.55, &o->transit_alt);
            return;
        }
    } else {
        o->rise = cross(fn, user, jd, 1, 1.05, h0, true);
        if (!o->rise) {
            o->never_up = true;
            o->transit = culmination(fn, user, jd, jd + 1.05, &o->transit_alt);
            return;
        }
        o->set = cross(fn, user, o->rise, 1, 1.05, h0, false);
    }
    double a = o->rise ? o->rise : jd - 0.5, b = o->set ? o->set : jd + 1.0;
    o->transit = culmination(fn, user, a, b, &o->transit_alt);
}

double cl_alt_body(double jd, void *user, double *ha)
{
    const cl_rts_body_t *b = (const cl_rts_body_t *)user;
    cl_body_t p;
    cl_body(b->body, jd, &p);
    cl_sky_t s;
    cl_sky_set(&s, jd, b->lat, b->lon);
    float enu[3], alt, az;
    cl_radec_to_enu(&s, p.ra, p.dec, b->body == CL_MOON ? p.dist_au : 0, enu);
    cl_enu_altaz(enu, &alt, &az);
    double h = s.lst - p.ra;
    h = cl_fmod_pos(h + CL_PI, CL_TWOPI) - CL_PI;
    *ha = h;
    return alt;
}

double cl_alt_star(double jd, void *user, double *ha)
{
    const cl_rts_star_t *st = (const cl_rts_star_t *)user;
    cl_sky_t s;
    cl_sky_set(&s, jd, st->lat, st->lon);
    float enu[3], alt, az;
    for (int i = 0; i < 3; i++)
        enu[i] = s.m_j2000_enu[i][0] * st->v[0] + s.m_j2000_enu[i][1] * st->v[1] + s.m_j2000_enu[i][2] * st->v[2];
    cl_enu_altaz(enu, &alt, &az);
    /* the hour angle, from the equator of date */
    float eq[3];
    for (int i = 0; i < 3; i++)
        eq[i] = s.m_date_enu[0][i] * enu[0] + s.m_date_enu[1][i] * enu[1] + s.m_date_enu[2][i] * enu[2];
    double ra = atan2f(eq[1], eq[0]);
    *ha = cl_fmod_pos(s.lst - ra + CL_PI, CL_TWOPI) - CL_PI;
    return alt;
}

/* ---------------------------------------------------------------------------
 * Galactic coordinates (the J2000 matrix of the IAU 1958 system as
 * transformed to FK5)
 * ------------------------------------------------------------------------- */

static const float GAL[3][3] = {
    { -0.0548755604f, -0.8734370902f, -0.4838350155f },
    { 0.4941094279f, -0.4448296300f, 0.7469822445f },
    { -0.8676661490f, -0.1980763734f, 0.4559837762f },
};

void cl_eq_to_gal(const float eq[3], float gal[3])
{
    for (int i = 0; i < 3; i++) gal[i] = GAL[i][0] * eq[0] + GAL[i][1] * eq[1] + GAL[i][2] * eq[2];
}

void cl_gal_to_eq(const float gal[3], float eq[3])
{
    for (int i = 0; i < 3; i++) eq[i] = GAL[0][i] * gal[0] + GAL[1][i] * gal[1] + GAL[2][i] * gal[2];
}
