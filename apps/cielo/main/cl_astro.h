/*
 * CIELO - where things are: time scales, the frames, the Sun, the Moon,
 * the planets, and when they rise, culminate and set.
 *
 * Time is a Julian date in UT (jd), in double; the dynamical time the
 * theories want is jd + delta T. Angles come out in radians unless the name
 * says otherwise.
 *
 *   Sun      Meeus, "Astronomical Algorithms" (2nd ed.), chapter 25, the
 *            low-precision theory: 0.01 degrees.
 *   Moon     the same book, chapter 47: the main periodic terms of
 *            Chapront's ELP-2000/82, about 60 in longitude and distance and
 *            45 in latitude.
 *   Planets  osculating-like elements with linear rates (the ecliptic and
 *            equinox of date) and the main mutual perturbations of Jupiter,
 *            Saturn and Uranus, after Paul Schlyter's well-known method,
 *            plus one step of light time.
 *   Stars    J2000 catalogue positions, precessed to the date (Meeus 21.2)
 *            with the nutation in longitude and obliquity (Meeus 22, the
 *            four main terms).
 *
 * The accuracy measured against JPL Horizons is in the README and in
 * tests/test_astro.c, which is where it is checked.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    CL_SUN = 0, CL_MOON, CL_MERCURY, CL_VENUS, CL_MARS, CL_JUPITER, CL_SATURN, CL_URANUS, CL_NEPTUNE,
    CL_NBODY
};

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

#define CL_JD_UNIX  2440587.5       /* 1970-01-01 0h UT */
#define CL_J2000    2451545.0

double cl_jd_from_unix(double t);
double cl_unix_from_jd(double jd);
/* TT - UT in seconds: Espenak and Meeus' polynomials, close to the
 * measured 69 s for the 2020s */
double cl_delta_t(double jd);
/* Greenwich mean sidereal time, degrees in [0, 360) (Meeus 12.4) */
double cl_gmst_deg(double jd);

/* ---------------------------------------------------------------------------
 * The sky of a moment for a place: everything a frame needs, computed once
 * per frame (cl_sky_set) and read by every star.
 * ------------------------------------------------------------------------- */

typedef struct {
    double jd;                  /* UT */
    double lat, lon;            /* the observer, radians, east positive */
    double lst;                 /* local apparent sidereal time, radians */
    double eps;                 /* true obliquity, radians */
    double dpsi;                /* nutation in longitude, radians */
    /* J2000 equatorial -> the local horizon (east, north, up), in float:
     * a star's unit vector times this is where it stands */
    float  m_j2000_enu[3][3];
    /* equator of date -> east, north, up */
    float  m_date_enu[3][3];
    double m_date_enu_d[3][3];
    /* the slow part, kept by cl_sky_update(): precession and nutation */
    double pn[3][3], pn_jde, pn_eps, pn_dpsi;
    bool   pn_ok;
} cl_sky_t;

void cl_sky_set(cl_sky_t *s, double jd, double lat_deg, double lon_deg);
/* The same, keeping the precession and the nutation of s's last call while
 * they move less than an arcsecond (an hour): what changes from one frame
 * to the next is only the sidereal time. s must have been zeroed once. */
void cl_sky_update(cl_sky_t *s, double jd, double lat_deg, double lon_deg);

/* A J2000 unit vector to the mean equator of date (precession only;
 * jde in dynamical time). */
void cl_precess(double jde, const double in[3], double out[3]);

/* ---------------------------------------------------------------------------
 * The bodies
 * ------------------------------------------------------------------------- */

typedef struct {
    double ra, dec;             /* apparent geocentric, equator of date */
    double dist_au;             /* from the Earth's centre */
    double r_au;                /* from the Sun (planets), 0 for the Sun */
    double elon, elat;          /* ecliptic of date */
    float  mag;
    float  phase;               /* illuminated fraction 0..1 */
    float  phase_angle;         /* Sun-body-Earth, radians */
    float  semidiam;            /* radians */
} cl_body_t;

void cl_body(int which, double jd, cl_body_t *out);

/* A direction of date (ra, dec) to the local east, north, up of the sky,
 * with the topocentric parallax of a body at dist_au (0: none). */
void cl_radec_to_enu(const cl_sky_t *s, double ra, double dec, double dist_au, float enu[3]);
/* east, north, up -> altitude and azimuth (from the north towards the east) */
void cl_enu_altaz(const float enu[3], float *alt, float *az);
/* back: the local direction -> J2000 equatorial (for the Milky Way and the
 * catalogues), by the transposed matrix */
void cl_enu_to_j2000(const cl_sky_t *s, const float enu[3], float out[3]);

/* Atmospheric refraction for an apparent altitude, radians (Saemundsson). */
float cl_refraction(float alt);

/* ---------------------------------------------------------------------------
 * Rise, culmination and set
 *
 * For whatever cl_rts_alt_fn says the altitude is at a given moment (a
 * star, a body, the Sun for twilights), searched from jd0 over 'span'
 * days. h0 is the altitude that counts as the horizon (-0.5667 degrees for a
 * star, -0.8333 for the Sun's upper limb, refraction included).
 * ------------------------------------------------------------------------- */

/* the altitude at jd, radians, and the hour angle there (-pi..pi) */
typedef double (*cl_alt_fn)(double jd, void *user, double *ha);

typedef struct {
    double rise, transit, set;  /* jd, 0 = none in the span */
    float  transit_alt;         /* radians */
    bool   always_up, never_up;
} cl_rts_t;

/* The events around jd: if the thing is up, the rise before it and the set
 * after; if it is down, the next rise and the set after that; the
 * culmination is the one in between (the highest point). */
void cl_rts(cl_alt_fn fn, void *user, double jd, double h0_rad, cl_rts_t *out);

/* A ready-made altitude for a body (user: a cl_rts_body_t). */
#define CL_RTS_KNOTS 84
typedef struct {
    int    body;
    double lat, lon;            /* degrees */
    /* filled as needed: the body's place every hour from k0, which a
     * search interpolates (the Moon's path bends by arcseconds in an hour) */
    double k0;
    float  ra[CL_RTS_KNOTS], dec[CL_RTS_KNOTS], par[CL_RTS_KNOTS];
    bool   have[CL_RTS_KNOTS];
} cl_rts_body_t;
double cl_alt_body(double jd, void *user, double *ha);

/* ... and for a J2000 direction (user: a cl_rts_star_t) */
typedef struct {
    float  v[3];                /* J2000 */
    double lat, lon;
    double ra, dec;             /* of date, worked out on the first call */
    bool   ok;
} cl_rts_star_t;
double cl_alt_star(double jd, void *user, double *ha);

/* The galactic pole and centre (J2000), for the Milky Way: J2000
 * equatorial -> galactic (l from the centre, b) */
void cl_eq_to_gal(const float eq[3], float gal[3]);
void cl_gal_to_eq(const float gal[3], float eq[3]);
