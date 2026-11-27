/*
 * CIELO - satellites: two-line elements, SGP4, and where one stands in the
 * sky of a place.
 *
 * SGP4 is the near-Earth model of Spacetrack Report #3 (Hoots and
 * Roehrich, 1980), a work of the US government, written here from its
 * equations with the corrections of Vallado, Crawford, Hujsak and Kelso,
 * "Revisiting Spacetrack Report #3" (AIAA 2006-6753), and the WGS-72
 * constants the elements are fitted with. Only the near-Earth branch: an
 * orbit of 225 minutes or more (SDP4, the deep-space one) is refused, and
 * none of those makes a pass worth watching. tests/test_astro.c checks the
 * propagation against the report's published test cases.
 *
 * Everything in double (cl_dmath), because the mean anomaly grows by
 * thousands of radians in the days a TLE is good for.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     name[25];
    uint32_t norad;
    double   epoch_jd;              /* UT */
    /* the elements as read, radians and radians per minute */
    double   incl, raan, ecc, argp, ma, n_kozai, bstar;
    /* sgp4init's constants */
    double   no, ao, con41, con42, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao,
             t2cof, t3cof, t4cof, t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf, aycof;
    bool     isimp;
    bool     ok;                    /* parsed and initialised */
    char     err[24];
} cl_sat_t;

/* Parse a TLE (the name line may be NULL) and initialise SGP4. */
bool cl_tle_parse(cl_sat_t *s, const char *name, const char *l1, const char *l2);

/* Position and velocity in the TEME frame, km and km/s, tsince minutes
 * from the epoch. false: the orbit decayed or the elements broke down. */
bool cl_sgp4(const cl_sat_t *s, double tsince, double r[3], double v[3]);

/* Where it is in the sky: azimuth from the north towards the east, and
 * elevation, radians; range km; whether the Sun lights it; jd in UT. */
typedef struct {
    double az, el, range;
    double lat, lon, alt_km;        /* the point under it, degrees and km */
    bool   sunlit;
    float  mag;                     /* a guess at its brightness (std_mag) */
} cl_look_t;

/* std_mag: the magnitude at 1000 km, half lit (the ISS -1.8; 99 if
 * unknown, which then gives 5 for anything lit). */
bool cl_sat_look(const cl_sat_t *s, double jd, double lat_deg, double lon_deg, double h_km, float std_mag,
                 cl_look_t *out);

/* The Sun's unit vector from the Earth, equator of date (good to 0.01
 * degree, plenty for a shadow). */
void cl_sun_vec(double jd, double v[3]);

/* A pass over the place: from rising above el_min to setting under it. */
typedef struct {
    double rise, top, set;          /* jd */
    float  rise_az, top_az, set_az, top_el;    /* radians */
    double vis_start, vis_end;      /* the part lit by the Sun with the sky dark, 0 none */
    float  mag;                     /* the brightest it gets while visible */
} cl_pass_t;

/* The passes from jd0 over 'days', up to max of them. Only those whose
 * highest point is above el_min. Returns how many. yield(ctx), if given,
 * runs every 16 propagations: it may sleep to let other tasks run, and
 * returns true to stop the search (what was found until then is
 * returned). */
int cl_passes(const cl_sat_t *s, double jd0, double days, double lat_deg, double lon_deg, double h_km,
              float el_min_rad, float std_mag, cl_pass_t *out, int max, bool (*yield)(void *), void *ctx);

/* SGP4 propagations so far (all threads), for measuring */
extern uint32_t cl_sgp4_evals;
