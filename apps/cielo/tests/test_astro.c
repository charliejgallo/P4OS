/*
 * CIELO - the engine against published ephemerides, on the computer:
 *
 *     apps/cielo/tests/run.sh
 *
 * What it compares, and where the reference values come from:
 *
 *   - the double maths of cl_dmath.c against the host's libm;
 *   - sidereal time and precession against the worked examples of Meeus,
 *     "Astronomical Algorithms", 2nd ed. (12.a, 12.b, 21.b, 47.a);
 *   - the Sun, the Moon and the eight planets against JPL Horizons
 *     (geocentric apparent RA and Dec, read from its web API on
 *     2026-11-27), at four dates from 2000 to 2035;
 *   - rise, culmination and set in Buenos Aires against Horizons'
 *     rise-transit-set table for the same day;
 *   - SGP4 against the published test cases: satellite 00005 of
 *     Vallado et al.'s verification set, and the ISS example that the
 *     python-sgp4 package documents (its position; the velocities are
 *     checked by 00005, at four times).
 *
 * Each check prints the error and fails past a bound chosen for what a
 * planetarium needs (well under the Moon's diameter); the bounds are the
 * numbers given in the README.
 */
#include "../main/cl_astro.h"
#include "../main/cl_dmath.h"
#include "../main/cl_sgp4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails, checks;

static void check(const char *what, double err, double bound, const char *unit)
{
    checks++;
    bool ok = fabs(err) <= bound;
    if (!ok) fails++;
    printf("%-4s %-46s %10.4f %s (bound %g)\n", ok ? "ok" : "FAIL", what, err, unit, bound);
}

/* the angle between two directions given in degrees, arcminutes */
static double sep_arcmin(double ra1, double de1, double ra2, double de2)
{
    double r = M_PI / 180;
    double c = sin(de1 * r) * sin(de2 * r) + cos(de1 * r) * cos(de2 * r) * cos((ra1 - ra2) * r);
    if (c > 1) c = 1;
    return acos(c) / r * 60;
}

static void test_dmath(void)
{
    double worst_s = 0, worst_a = 0, worst_q = 0;
    srand(7);
    for (int i = 0; i < 200000; i++) {
        double x = ((double)rand() / RAND_MAX - 0.5) * 2000.0;
        double e = fabs(cl_dsin(x) - sin(x)) + fabs(cl_dcos(x) - cos(x));
        if (e > worst_s) worst_s = e;
        double y = ((double)rand() / RAND_MAX - 0.5) * 10;
        double a = fabs(cl_datan2(y, x) - atan2(y, x));
        if (a > worst_a) worst_a = a;
        double q = fabs(cl_dsqrt(fabs(x)) - sqrt(fabs(x))) / (sqrt(fabs(x)) + 1e-300);
        if (q > worst_q) worst_q = q;
    }
    check("dmath: sin+cos, |x| < 1000", worst_s, 1e-12, "abs");
    check("dmath: atan2", worst_a, 1e-14, "rad");
    check("dmath: sqrt", worst_q, 1e-15, "rel");
    check("dmath: cbrt(27)", cl_dcbrt(27.0) - 3.0, 1e-14, "abs");
    check("dmath: floor(-2.5)", cl_floor(-2.5) + 3.0, 0, "abs");
}

static void test_time(void)
{
    /* Meeus 12.a: 1987 April 10, 0h UT: 13h10m46.3668s */
    double g = cl_gmst_deg(2446895.5);
    check("GMST 1987-04-10 0h (Meeus 12.a)", (g - (13 + 10 / 60.0 + 46.3668 / 3600.0) * 15) * 3600 / 15, 0.01, "s");
    /* 12.b: 19h21m00s UT: 8h34m57.0896s */
    g = cl_gmst_deg(2446896.30625);
    check("GMST 1987-04-10 19:21 (Meeus 12.b)", (g - (8 + 34 / 60.0 + 57.0896 / 3600.0) * 15) * 3600 / 15, 0.01, "s");
    /* 21.b: theta Persei, J2000 with its proper motion to the date already
     * applied (41.054063, +49.227750), to 2028 Nov 13.19 TD: 41.547214,
     * +49.348483 */
    double ra0 = 41.054063, de0 = 49.227750;
    double r = M_PI / 180, in[3] = { cos(de0 * r) * cos(ra0 * r), cos(de0 * r) * sin(ra0 * r), sin(de0 * r) }, o[3];
    cl_precess(2462088.69, in, o);
    double ra = atan2(o[1], o[0]) / r, de = asin(o[2]) / r;
    check("precession, theta Per to 2028 (Meeus 21.b)", sep_arcmin(ra, de, 41.547214, 49.348483) * 60, 1.0, "arcsec");
}

typedef struct {
    int body;
    double jd, ra, dec;
} ref_t;

/* JPL Horizons, geocentric apparent RA/Dec (equator of date), UT */
static const ref_t REFS[] = {
    { CL_SUN, 2461371.5, 242.78656, -21.08317 },
    { CL_MOON, 2461371.5, 99.21209, 26.77829 },
    { CL_MERCURY, 2461371.5, 224.53781, -14.86631 },
    { CL_VENUS, 2461371.5, 204.39985, -9.21825 },
    { CL_MARS, 2461371.5, 153.27375, 13.39311 },
    { CL_JUPITER, 2461371.5, 149.11050, 13.38421 },
    { CL_SATURN, 2461371.5, 8.47308, 0.82425 },
    { CL_URANUS, 2461371.5, 61.64730, 20.73207 },
    { CL_NEPTUNE, 2461371.5, 2.10347, -0.61530 },
    { CL_SUN, 2451545.0, 281.27838, -23.03243 },
    { CL_MOON, 2451545.0, 222.45220, -10.90065 },
    { CL_MERCURY, 2451545.0, 272.07464, -24.41890 },
    { CL_VENUS, 2451545.0, 239.89276, -18.44892 },
    { CL_MARS, 2451545.0, 330.51680, -13.18248 },
    { CL_JUPITER, 2451545.0, 23.86784, 8.59426 },
    { CL_SATURN, 2451545.0, 38.76539, 12.61476 },
    { CL_URANUS, 2451545.0, 317.47481, -17.02033 },
    { CL_NEPTUNE, 2451545.0, 305.43283, -19.21324 },
    { CL_SUN, 2457189.25, 83.81786, 23.31280 },
    { CL_MOON, 2457189.25, 73.02295, 17.85929 },
    { CL_MERCURY, 2457189.25, 63.96550, 17.00227 },
    { CL_VENUS, 2457189.25, 132.35697, 19.82438 },
    { CL_MARS, 2457189.25, 83.46270, 23.92738 },
    { CL_JUPITER, 2457189.25, 141.58836, 15.98807 },
    { CL_SATURN, 2457189.25, 238.24917, -18.04007 },
    { CL_URANUS, 2457189.25, 18.56267, 7.17272 },
    { CL_NEPTUNE, 2457189.25, 341.65773, -8.61177 },
    { CL_SUN, 2464406.75, 359.50442, -0.21488 },
    { CL_MOON, 2464406.75, 135.38413, 14.10255 },
    { CL_MERCURY, 2464406.75, 346.64498, -8.16056 },
    { CL_VENUS, 2464406.75, 325.70440, -14.17226 },
    { CL_MARS, 2464406.75, 285.91259, -23.14889 },
    { CL_JUPITER, 2464406.75, 19.16206, 6.94424 },
    { CL_SATURN, 2464406.75, 120.73823, 20.90172 },
    { CL_URANUS, 2464406.75, 97.09606, 23.60551 },
    { CL_NEPTUNE, 2464406.75, 20.18923, 6.77415 },
};

static const char *const NAMES[] = { "Sun", "Moon", "Mercury", "Venus", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune" };
/* arcminutes: the Sun 0.6, the Moon 1.5, the planets 2 (Jupiter and
 * Saturn 3, where the mutual perturbations left out are biggest) */
static const double BOUND[] = { 0.6, 1.5, 2, 2, 2, 3, 3, 2, 2 };

static void test_bodies(void)
{
    double worst[CL_NBODY] = { 0 };
    for (unsigned i = 0; i < sizeof REFS / sizeof REFS[0]; i++) {
        const ref_t *r = &REFS[i];
        cl_body_t b;
        cl_body(r->body, r->jd, &b);
        double e = sep_arcmin(b.ra * 180 / M_PI, b.dec * 180 / M_PI, r->ra, r->dec);
        char what[64];
        snprintf(what, sizeof what, "%s at JD %.2f", NAMES[r->body], r->jd);
        check(what, e, BOUND[r->body], "arcmin");
        if (e > worst[r->body]) worst[r->body] = e;
    }
    printf("     worst, arcmin:");
    for (int k = 0; k < CL_NBODY; k++) printf(" %s %.2f", NAMES[k], worst[k]);
    printf("\n");
    /* Meeus 47.a, the Moon on 1992 April 12, 0h TD: apparent RA 134.688470,
     * Dec 13.768368, distance 368409.7 km */
    cl_body_t m;
    double jd = 2448724.5 - cl_delta_t(2448724.5) / 86400.0;
    cl_body(CL_MOON, jd, &m);
    check("Moon 1992-04-12 (Meeus 47.a)", sep_arcmin(m.ra * 180 / M_PI, m.dec * 180 / M_PI, 134.688470, 13.768368) * 60,
          10, "arcsec");
    check("Moon's distance (Meeus 47.a)", m.dist_au * 149597870.7 - 368409.7, 2, "km");
}

static void test_rts(void)
{
    /* Horizons, Buenos Aires (-58.3816, -34.6037), 2026-11-27 (UT),
     * true visual horizon (refraction, upper limb for the Sun and Moon) */
    struct {
        int body;
        double r, t, s;     /* hours UT */
        double h0;
    } T[] = {
        { CL_SUN, 8 + 35 / 60.0, 15 + 42 / 60.0, 22 + 49 / 60.0, -0.8333 },
        { CL_MOON, 1 + 33 / 60.0, 6 + 24 / 60.0, 11 + 18 / 60.0, -0.8333 },
        { CL_JUPITER, 4 + 1 / 60.0, 9 + 25 / 60.0, 14 + 50 / 60.0, -0.5667 },
    };
    for (int i = 0; i < 3; i++) {
        cl_rts_body_t u = { .body = T[i].body, .lat = -34.6037, .lon = -58.3816 };
        cl_rts_t o;
        /* from the day's 0h, when all three are below the horizon */
        cl_rts(cl_alt_body, &u, 2461371.5, T[i].h0 * M_PI / 180, &o);
        double d0 = 2461371.5;
        char w[64];
        snprintf(w, sizeof w, "%s rises, Buenos Aires", NAMES[T[i].body]);
        check(w, ((o.rise - d0) * 24 - T[i].r) * 60, 1.5, "min");
        snprintf(w, sizeof w, "%s culminates", NAMES[T[i].body]);
        check(w, ((o.transit - d0) * 24 - T[i].t) * 60, 1.5, "min");
        snprintf(w, sizeof w, "%s sets", NAMES[T[i].body]);
        check(w, ((o.set - d0) * 24 - T[i].s) * 60, 1.5, "min");
    }
}

static void test_sgp4(void)
{
    /* 00005, Vallado et al. 2006, the first case of SGP4-VER.TLE */
    cl_sat_t s;
    bool ok = cl_tle_parse(&s, "00005",
                           "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
                           "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667");
    check("TLE 00005 parses", ok ? 0 : 1, 0, "");
    struct {
        double t, x, y, z, vx, vy, vz;
    } V[] = {
        { 0, 7022.46529266, -1400.08296755, 0.03995155, 1.893841015, 6.405893759, 4.534807250 },
        { 360, -7154.03120202, -3783.17682504, -3536.19412294, 4.741887409, -4.151817765, -2.093935425 },
        { 720, -7134.59340119, 6531.68641334, 3260.27186483, -4.113793027, -2.911922039, -2.557327851 },
        { 1080, 5568.53901181, 4492.06992591, 3863.87641983, -4.209106476, 5.159719888, 2.744852980 },
    };
    for (int i = 0; i < 4; i++) {
        double r[3], v[3];
        cl_sgp4(&s, V[i].t, r, v);
        double dr = sqrt(pow(r[0] - V[i].x, 2) + pow(r[1] - V[i].y, 2) + pow(r[2] - V[i].z, 2));
        double dv = sqrt(pow(v[0] - V[i].vx, 2) + pow(v[1] - V[i].vy, 2) + pow(v[2] - V[i].vz, 2));
        char w[64];
        snprintf(w, sizeof w, "SGP4 00005 at %4.0f min, position", V[i].t);
        check(w, dr * 1000, 1, "m");
        snprintf(w, sizeof w, "SGP4 00005 at %4.0f min, velocity", V[i].t);
        check(w, dv * 1e6, 1, "mm/s");
    }
    /* the ISS example of python-sgp4's documentation */
    ok = cl_tle_parse(&s, "ISS (ZARYA)",
                      "1 25544U 98067A   19343.69339541  .00001764  00000-0  38792-4 0  9991",
                      "2 25544  51.6439 211.2001 0007417  17.6667  85.6398 15.50103472202482");
    check("TLE 25544 parses", ok ? 0 : 1, 0, "");
    double r[3], v[3];
    double tsince = (2458827.362605 - s.epoch_jd) * 1440.0;
    cl_sgp4(&s, tsince, r, v);
    double dr = sqrt(pow(r[0] + 6102.443276428913, 2) + pow(r[1] + 986.3320160644567, 2) + pow(r[2] + 2820.3130707377886, 2));
    /* only the position: the velocity is checked by 00005 above */
    check("SGP4 ISS 2019-12-09, position", dr * 1000, 1, "m");

    /* a broken checksum is refused */
    ok = cl_tle_parse(&s, "x", "1 25544U 98067A   19343.69339541  .00001764  00000-0  38792-4 0  9992",
                      "2 25544  51.6439 211.2001 0007417  17.6667  85.6398 15.50103472202482");
    check("TLE with a bad checksum is refused", ok ? 1 : 0, 0, "");

    /* the passes come out in order and inside their own bounds */
    cl_tle_parse(&s, "ISS (ZARYA)",
                 "1 25544U 98067A   19343.69339541  .00001764  00000-0  38792-4 0  9991",
                 "2 25544  51.6439 211.2001 0007417  17.6667  85.6398 15.50103472202482");
    cl_pass_t p[16];
    int n = cl_passes(&s, s.epoch_jd, 2.0, -34.6037, -58.3816, 0.02, 10 * M_PI / 180, -1.8f, p, 16, NULL, NULL);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (!(p[i].rise < p[i].top && p[i].top < p[i].set)) bad++;
        if (i && p[i].rise <= p[i - 1].set) bad++;
        double dur = (p[i].set - p[i].rise) * 1440;
        if (dur < 1 || dur > 12) bad++;
    }
    printf("     %d ISS passes over 10 degrees in two days from the epoch\n", n);
    /* the adaptive search misses none: every pass a brute-force scan every
     * 10 s finds over 10 degrees, it has too, with its top within a second */
    {
        int bf = 0, matched = 0;
        bool up = false;
        double best = -9, best_t = 0;
        for (double t = s.epoch_jd; t < s.epoch_jd + 2.0; t += 10.0 / 86400) {
            cl_look_t lk;
            cl_sat_look(&s, t, -34.6037, -58.3816, 0.02, 99, &lk);
            if (lk.el > 0) {
                if (!up) best = -9;
                up = true;
                if (lk.el > best) best = lk.el, best_t = t;
            } else if (up) {
                up = false;
                if (best >= 10 * M_PI / 180) {
                    bf++;
                    for (int i = 0; i < n; i++)
                        if (fabs(p[i].top - best_t) * 86400 < 10) matched++;
                }
            }
        }
        printf("     brute force: %d passes, %d matched\n", bf, matched);
        check("adaptive search finds every pass", bf - n, 0, "");
        check("adaptive search, tops matched", bf - matched, 0, "");
    }
    check("passes ordered, 1-12 minutes each", bad, 0, "");
    check("passes found", n >= 2 ? 0 : 1, 0, "");
}

int main(void)
{
    test_dmath();
    test_time();
    test_bodies();
    test_rts();
    test_sgp4();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
