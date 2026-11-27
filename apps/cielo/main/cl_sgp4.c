/*
 * CIELO - SGP4 near-Earth propagation and the look from a place (see
 * cl_sgp4.h).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cl_sgp4.h"
#include "cl_astro.h"
#include "cl_dmath.h"

#include <math.h>
#include <string.h>

/* WGS-72, the constants the elements are fitted with */
#define MU          398600.8
#define RE          6378.135
#define J2          0.001082616
#define J3          (-0.00000253881)
#define J4          (-0.00000165597)
#define J3OJ2       (J3 / J2)
#define X2O3        (2.0 / 3.0)

static double s_xke;    /* 60 / sqrt(RE^3 / MU), set once */

static double xke(void)
{
    if (s_xke == 0) s_xke = 60.0 / cl_dsqrt(RE * RE * RE / MU);
    return s_xke;
}

/* x^(2/3) */
static double pow23(double x)
{
    double c = cl_dcbrt(x);
    return c * c;
}

/* ---------------------------------------------------------------------------
 * The TLE
 * ------------------------------------------------------------------------- */

/* a fixed-width field as a number; implied is the TLE's "12345-6" form
 * (a mantissa with an implied leading point and an exponent) */
static double field(const char *l, int from, int len, bool implied)
{
    char b[24];
    int n = 0;
    for (int i = 0; i < len && l[from + i] && n < 22; i++) b[n++] = l[from + i];
    b[n] = 0;
    if (!implied) {
        /* a plain decimal: sign, digits, point, digits */
        double v = 0, scale = 0;
        int sign = 1;
        bool any = false;
        for (int i = 0; b[i]; i++) {
            char c = b[i];
            if (c == '-') sign = -1;
            else if (c == '.') scale = 1;
            else if (c >= '0' && c <= '9') {
                any = true;
                v = v * 10 + (c - '0');
                if (scale) scale *= 10;
            }
        }
        (void)any;
        if (scale) v /= scale;
        return sign * v;
    }
    /* " 12345-4" or "-11606-4": sign, five digits, the exponent */
    int i = 0, sign = 1;
    while (b[i] == ' ') i++;
    if (b[i] == '-' || b[i] == '+') sign = b[i++] == '-' ? -1 : 1;
    double m = 0, scale = 1;
    while (b[i] >= '0' && b[i] <= '9') {
        m = m * 10 + (b[i++] - '0');
        scale *= 10;
    }
    m /= scale;
    int esign = 1, ex = 0;
    if (b[i] == '-' || b[i] == '+') esign = b[i++] == '-' ? -1 : 1;
    while (b[i] >= '0' && b[i] <= '9') ex = ex * 10 + (b[i++] - '0');
    double p = 1;
    for (int k = 0; k < ex; k++) p *= 10;
    return sign * (esign > 0 ? m * p : m / p);
}

static bool checksum_ok(const char *l)
{
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        char c = l[i];
        if (!c) return false;
        if (c >= '0' && c <= '9') sum += c - '0';
        else if (c == '-') sum += 1;
    }
    return l[68] >= '0' && l[68] <= '9' && sum % 10 == l[68] - '0';
}

static bool init(cl_sat_t *s);

bool cl_tle_parse(cl_sat_t *s, const char *name, const char *l1, const char *l2)
{
    memset(s, 0, sizeof *s);
    if (strlen(l1) < 69 || strlen(l2) < 69 || l1[0] != '1' || l2[0] != '2') {
        strcpy(s->err, "bad lines");
        return false;
    }
    if (!checksum_ok(l1) || !checksum_ok(l2)) {
        strcpy(s->err, "checksum");
        return false;
    }
    if (name) {
        int n = 0;
        const char *p = name;
        if (p[0] == '0' && p[1] == ' ') p += 2;         /* the "0 NAME" form */
        while (*p && *p != '\r' && *p != '\n' && n < (int)sizeof s->name - 1) s->name[n++] = *p++;
        while (n > 0 && s->name[n - 1] == ' ') n--;
        s->name[n] = 0;
    }
    s->norad = (uint32_t)field(l1, 2, 5, false);
    int yy = (int)field(l1, 18, 2, false);
    double day = field(l1, 20, 12, false);
    int year = yy < 57 ? 2000 + yy : 1900 + yy;
    /* JD of January 0.0 of that year */
    int y = year - 1;
    double jan0 = 1721424.5 + 365.0 * y + (double)(y / 4 - y / 100 + y / 400);
    s->epoch_jd = jan0 + day;
    s->bstar = field(l1, 53, 8, true);
    s->incl = field(l2, 8, 8, false) * CL_D2R;
    s->raan = field(l2, 17, 8, false) * CL_D2R;
    {
        char e[10] = "0.";
        memcpy(e + 2, l2 + 26, 7);
        e[9] = 0;
        s->ecc = field(e, 0, 9, false);
    }
    s->argp = field(l2, 34, 8, false) * CL_D2R;
    s->ma = field(l2, 43, 8, false) * CL_D2R;
    s->n_kozai = field(l2, 52, 11, false) * CL_TWOPI / 1440.0;
    if (s->n_kozai <= 0) {
        strcpy(s->err, "mean motion");
        return false;
    }
    return init(s);
}

/* sgp4init and initl, near-Earth only */
static bool init(cl_sat_t *s)
{
    const double ke = xke();
    double ecco = s->ecc, inclo = s->incl;
    double eccsq = ecco * ecco, omeosq = 1.0 - eccsq, rteosq = cl_dsqrt(omeosq);
    double cosio = cl_dcos(inclo), cosio2 = cosio * cosio;

    /* un-Kozai the mean motion */
    double ak = pow23(ke / s->n_kozai);
    double d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    double no = s->n_kozai / (1.0 + del);
    double ao = pow23(ke / no);
    double sinio = cl_dsin(inclo), po = ao * omeosq, con42 = 1.0 - 5.0 * cosio2;
    double con41 = -con42 - cosio2 - cosio2;
    double posq = po * po, rp = ao * (1.0 - ecco);
    s->no = no;
    s->ao = ao;
    s->con41 = con41;
    s->con42 = con42;

    if (CL_TWOPI / no >= 225.0) {
        strcpy(s->err, "deep space");
        return false;
    }
    if (omeosq <= 0 || no <= 0) {
        strcpy(s->err, "elements");
        return false;
    }

    s->isimp = rp < 220.0 / RE + 1.0;
    double ss = 78.0 / RE + 1.0, qzms2t = cl_dpowi((120.0 - 78.0) / RE, 4);
    double sfour = ss, qzms24 = qzms2t;
    double perige = (rp - 1.0) * RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = cl_dpowi((120.0 - sfour) / RE, 4);
        sfour = sfour / RE + 1.0;
    }
    double pinvsq = 1.0 / posq;
    double tsi = 1.0 / (ao - sfour);
    double eta = ao * ecco * tsi;
    double etasq = eta * eta, eeta = ecco * eta;
    double psisq = cl_dabs(1.0 - etasq);
    double coef = qzms24 * cl_dpowi(tsi, 4);
    /* psisq^3.5 */
    double coef1 = coef / (psisq * psisq * psisq * cl_dsqrt(psisq));
    double cc2 = coef1 * no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                               0.375 * J2 * tsi / psisq * con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    double bstar = s->bstar;
    s->cc1 = bstar * cc2;
    double cc3 = 0;
    if (ecco > 1.0e-4) cc3 = -2.0 * coef * tsi * J3OJ2 * no * sinio / ecco;
    s->x1mth2 = 1.0 - cosio2;
    s->cc4 = 2.0 * no * coef1 * ao * omeosq *
             (eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) -
              J2 * tsi / (ao * psisq) *
                  (-3.0 * con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                   0.75 * s->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cl_dcos(2.0 * s->argp)));
    s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    double cosio4 = cosio2 * cosio2;
    double temp1 = 1.5 * J2 * pinvsq * no;
    double temp2 = 0.5 * temp1 * J2 * pinvsq;
    double temp3 = -0.46875 * J4 * pinvsq * pinvsq * no;
    s->mdot = no + 0.5 * temp1 * rteosq * con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    s->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                 temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    double xhdot1 = -temp1 * cosio;
    s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    s->omgcof = bstar * cc3 * cl_dcos(s->argp);
    s->xmcof = 0;
    if (ecco > 1.0e-4) s->xmcof = -X2O3 * coef * bstar / eeta;
    s->nodecf = 3.5 * omeosq * xhdot1 * s->cc1;
    s->t2cof = 1.5 * s->cc1;
    if (cl_dabs(cosio + 1.0) > 1.5e-12) s->xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
    else s->xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;
    s->aycof = -0.5 * J3OJ2 * sinio;
    double dm = 1.0 + eta * cl_dcos(s->ma);
    s->delmo = dm * dm * dm;
    s->sinmao = cl_dsin(s->ma);
    s->x7thm1 = 7.0 * cosio2 - 1.0;
    s->eta = eta;
    if (!s->isimp) {
        double cc1sq = s->cc1 * s->cc1;
        s->d2 = 4.0 * ao * tsi * cc1sq;
        double temp = s->d2 * tsi * s->cc1 / 3.0;
        s->d3 = (17.0 * ao + sfour) * temp;
        s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->cc1;
        s->t3cof = s->d2 + 2.0 * cc1sq;
        s->t4cof = 0.25 * (3.0 * s->d3 + s->cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
        s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * s->cc1 * s->d3 + 6.0 * s->d2 * s->d2 + 15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
    }
    s->ok = true;
    return true;
}

bool cl_sgp4(const cl_sat_t *s, double t, double r[3], double v[3])
{
    if (!s->ok) return false;
    const double ke = xke();
    double xmdf = s->ma + s->mdot * t;
    double argpdf = s->argp + s->argpdot * t;
    double nodedf = s->raan + s->nodedot * t;
    double argpm = argpdf, mm = xmdf, t2 = t * t;
    double nodem = nodedf + s->nodecf * t2;
    double tempa = 1.0 - s->cc1 * t;
    double tempe = s->bstar * s->cc4 * t;
    double templ = s->t2cof * t2;
    if (!s->isimp) {
        double delomg = s->omgcof * t;
        double dmt = 1.0 + s->eta * cl_dcos(xmdf);
        double delm = s->xmcof * (dmt * dmt * dmt - s->delmo);
        double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        double t3 = t2 * t, t4 = t3 * t;
        tempa = tempa - s->d2 * t2 - s->d3 * t3 - s->d4 * t4;
        tempe = tempe + s->bstar * s->cc5 * (cl_dsin(mm) - s->sinmao);
        templ = templ + s->t3cof * t3 + t4 * (s->t4cof + t * s->t5cof);
    }
    double nm = s->no, em = s->ecc, inclm = s->incl;
    double am = pow23(ke / nm) * tempa * tempa;
    nm = ke / (am * cl_dsqrt(am));
    em = em - tempe;
    if (em >= 1.0 || em < -0.001 || am < 0.95) return false;
    if (em < 1.0e-6) em = 1.0e-6;
    mm = mm + s->no * templ;
    double xlm = mm + argpm + nodem;
    nodem = cl_fmod_pos(nodem, CL_TWOPI);
    argpm = cl_fmod_pos(argpm, CL_TWOPI);
    xlm = cl_fmod_pos(xlm, CL_TWOPI);
    mm = cl_fmod_pos(xlm - argpm - nodem, CL_TWOPI);

    double sinip, cosip;
    cl_dsincos(inclm, &sinip, &cosip);
    double ep = em, argpp = argpm, nodep = nodem, mp = mm, xincp = inclm;

    /* long-period periodics */
    double sa, ca;
    cl_dsincos(argpp, &sa, &ca);
    double axnl = ep * ca;
    double temp = 1.0 / (am * (1.0 - ep * ep));
    double aynl = ep * sa + temp * s->aycof;
    double xl = mp + argpp + nodep + temp * s->xlcof * axnl;

    /* Kepler's equation */
    double u = cl_fmod_pos(xl - nodep, CL_TWOPI);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 1;
    for (int ktr = 1; cl_dabs(tem5) >= 1.0e-12 && ktr <= 10; ktr++) {
        cl_dsincos(eo1, &sineo1, &coseo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (cl_dabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        eo1 = eo1 + tem5;
    }

    /* short-period periodics */
    double ecose = axnl * coseo1 + aynl * sineo1;
    double esine = axnl * sineo1 - aynl * coseo1;
    double el2 = axnl * axnl + aynl * aynl;
    double pl = am * (1.0 - el2);
    if (pl < 0.0) return false;
    double rl = am * (1.0 - ecose);
    double rdotl = cl_dsqrt(am) * esine / rl;
    double rvdotl = cl_dsqrt(pl) / rl;
    double betal = cl_dsqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = cl_datan2(sinu, cosu);
    double sin2u = (cosu + cosu) * sinu;
    double cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    double temp1 = 0.5 * J2 * temp;
    double temp2 = temp1 * temp;

    double mrt = rl * (1.0 - 1.5 * temp2 * betal * s->con41) + 0.5 * temp1 * s->x1mth2 * cos2u;
    su = su - 0.25 * temp2 * s->x7thm1 * sin2u;
    double xnode = nodep + 1.5 * temp2 * cosip * sin2u;
    double xinc = xincp + 1.5 * temp2 * cosip * sinip * cos2u;
    double mvt = rdotl - nm * temp1 * s->x1mth2 * sin2u / ke;
    double rvdot = rvdotl + nm * temp1 * (s->x1mth2 * cos2u + 1.5 * s->con41) / ke;

    double sinsu, cossu, snod, cnod, sini, cosi;
    cl_dsincos(su, &sinsu, &cossu);
    cl_dsincos(xnode, &snod, &cnod);
    cl_dsincos(xinc, &sini, &cosi);
    double xmx = -snod * cosi, xmy = cnod * cosi;
    double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
    double vx = xmx * cossu - cnod * sinsu, vy = xmy * cossu - snod * sinsu, vz = sini * cossu;
    const double vkmps = RE * ke / 60.0;
    r[0] = mrt * ux * RE;
    r[1] = mrt * uy * RE;
    r[2] = mrt * uz * RE;
    v[0] = (mvt * ux + rvdot * vx) * vkmps;
    v[1] = (mvt * uy + rvdot * vy) * vkmps;
    v[2] = (mvt * uz + rvdot * vz) * vkmps;
    return mrt >= 1.0;
}

/* ---------------------------------------------------------------------------
 * The look from a place
 * ------------------------------------------------------------------------- */

/* GMST in the IAU 1982 form SGP4's frame goes with, radians */
static double gstime(double jdut1)
{
    double tut1 = (jdut1 - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
                  (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
    return cl_fmod_pos(temp * CL_D2R / 240.0, CL_TWOPI);
}

void cl_sun_vec(double jd, double v[3])
{
    double n = jd - CL_J2000;
    double l = cl_fmod_pos(280.460 + 0.9856474 * n, 360.0);
    double g = cl_fmod_pos(357.528 + 0.9856003 * n, 360.0) * CL_D2R;
    double lam = (l + 1.915 * cl_dsin(g) + 0.020 * cl_dsin(2 * g)) * CL_D2R;
    double eps = (23.439 - 0.0000004 * n) * CL_D2R;
    double sl, cl, se, ce;
    cl_dsincos(lam, &sl, &cl);
    cl_dsincos(eps, &se, &ce);
    v[0] = cl;
    v[1] = ce * sl;
    v[2] = se * sl;
}

/* the observer, earth-fixed, km (WGS-84) */
static void site(double lat, double lon, double h, double o[3])
{
    const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    double sp, cp, sl, cl;
    cl_dsincos(lat, &sp, &cp);
    cl_dsincos(lon, &sl, &cl);
    double n = a / cl_dsqrt(1 - e2 * sp * sp);
    o[0] = (n + h) * cp * cl;
    o[1] = (n + h) * cp * sl;
    o[2] = (n * (1 - e2) + h) * sp;
}

bool cl_sat_look(const cl_sat_t *s, double jd, double lat_deg, double lon_deg, double h_km, float std_mag,
                 cl_look_t *o)
{
    double r[3], v[3];
    double tsince = (jd - s->epoch_jd) * 1440.0;
    if (!cl_sgp4(s, tsince, r, v)) return false;
    double g = gstime(jd), sg, cg;
    cl_dsincos(g, &sg, &cg);
    /* TEME -> earth-fixed (no polar motion) */
    double e[3] = { cg * r[0] + sg * r[1], -sg * r[0] + cg * r[1], r[2] };
    double lat = lat_deg * CL_D2R, lon = lon_deg * CL_D2R;
    double ob[3];
    site(lat, lon, h_km, ob);
    double d[3] = { e[0] - ob[0], e[1] - ob[1], e[2] - ob[2] };
    double sp, cp, sl, cl;
    cl_dsincos(lat, &sp, &cp);
    cl_dsincos(lon, &sl, &cl);
    double east = -sl * d[0] + cl * d[1];
    double north = -sp * cl * d[0] - sp * sl * d[1] + cp * d[2];
    double up = cp * cl * d[0] + cp * sl * d[1] + sp * d[2];
    double rng = cl_dsqrt(east * east + north * north + up * up);
    o->range = rng;
    o->el = cl_dasin(up / rng);
    o->az = cl_fmod_pos(cl_datan2(east, north), CL_TWOPI);
    /* the sub-satellite point, spherical: near enough for a map pin */
    double rr = cl_dsqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    o->lat = cl_dasin(e[2] / rr) * CL_R2D;
    o->lon = cl_datan2(e[1], e[0]) * CL_R2D;
    o->alt_km = rr - 6371.0;

    /* lit: outside the Earth's cylinder of shadow */
    double sv[3];
    cl_sun_vec(jd, sv);
    double dot = r[0] * sv[0] + r[1] * sv[1] + r[2] * sv[2];
    double px = r[0] - dot * sv[0], py = r[1] - dot * sv[1], pz = r[2] - dot * sv[2];
    o->sunlit = dot > 0 || cl_dsqrt(px * px + py * py + pz * pz) > 6371.0;

    /* brightness: a diffuse sphere, scaled from 1000 km and half lit */
    o->mag = 99;
    if (o->sunlit) {
        /* the phase angle: Sun -> satellite -> observer (ECI is enough) */
        double obs_eci[3] = { cg * ob[0] - sg * ob[1], sg * ob[0] + cg * ob[1], ob[2] };
        double so[3] = { obs_eci[0] - r[0], obs_eci[1] - r[1], obs_eci[2] - r[2] };
        double sn = cl_dsqrt(so[0] * so[0] + so[1] * so[1] + so[2] * so[2]);
        double cph = (so[0] * sv[0] + so[1] * sv[1] + so[2] * sv[2]) / sn;
        /* the angle at the satellite between the Sun and us */
        double ph = cl_dacos(cph);
        double fph = (cl_dsin(ph) + (CL_PI - ph) * cl_dcos(ph)) / CL_PI;
        float sm = std_mag < 90 ? std_mag : 4.0f;
        if (fph < 1e-4) fph = 1e-4;
        o->mag = sm + 5.0f * log10f((float)(rng / 1000.0)) - 2.5f * log10f((float)(fph * CL_PI));
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Passes
 * ------------------------------------------------------------------------- */

static double el_at(const cl_sat_t *s, double jd, double lat, double lon, double h, bool *ok)
{
    cl_look_t lk;
    *ok = cl_sat_look(s, jd, lat, lon, h, 99, &lk);
    return *ok ? lk.el : -2;
}

static double sun_alt(double jd, double lat, double lon)
{
    double v[3];
    cl_sun_vec(jd, v);
    double g = gstime(jd) + lon * CL_D2R;
    double sp, cp, sg, cg;
    cl_dsincos(lat * CL_D2R, &sp, &cp);
    cl_dsincos(g, &sg, &cg);
    double up = cp * cg * v[0] + cp * sg * v[1] + sp * v[2];
    return cl_dasin(up);
}

/* whether it can be seen at jd: above the horizon, lit, the sky dark */
static bool seen(const cl_sat_t *s, double jd, double lat, double lon, double h, float std_mag, float *mag)
{
    cl_look_t lk;
    if (!cl_sat_look(s, jd, lat, lon, h, std_mag, &lk)) return false;
    *mag = lk.mag;
    return lk.el > 0 && lk.sunlit && sun_alt(jd, lat, lon) < -6.0 * CL_D2R;
}

int cl_passes(const cl_sat_t *s, double jd0, double days, double lat, double lon, double h, float el_min,
              float std_mag, cl_pass_t *out, int max, volatile const bool *stop)
{
    if (!s->ok || max <= 0) return 0;
    const double step = 30.0 / 86400.0;
    int n = 0;
    bool ok;
    double t = jd0, e = el_at(s, t, lat, lon, h, &ok);
    if (!ok) return 0;
    /* a pass already under way: start from its rise */
    if (e > 0) {
        double b = t;
        for (int i = 0; i < 60 && e > 0; i++) {
            b -= step;
            e = el_at(s, b, lat, lon, h, &ok);
        }
        t = b;
    }
    double end = jd0 + days;
    while (t < end && n < max) {
        if (stop && *stop) break;
        double t2 = t + step, e2 = el_at(s, t2, lat, lon, h, &ok);
        if (!ok) break;
        if (e <= 0 && e2 > 0) {
            /* rising: bisect it, then walk to the set */
            double a = t, b = t2;
            for (int k = 0; k < 15; k++) {
                double m = 0.5 * (a + b);
                if (el_at(s, m, lat, lon, h, &ok) > 0) b = m;
                else a = m;
            }
            cl_pass_t p;
            memset(&p, 0, sizeof p);
            p.rise = b;
            double tt = t2, ee = e2, best = e2, best_t = t2;
            while (ee > 0 && tt < b + 0.05) {
                double tn = tt + step, en = el_at(s, tn, lat, lon, h, &ok);
                if (!ok) break;
                if (en > best) {
                    best = en;
                    best_t = tn;
                }
                if (en <= 0) {
                    double a2 = tt, b2 = tn;
                    for (int k = 0; k < 15; k++) {
                        double m = 0.5 * (a2 + b2);
                        if (el_at(s, m, lat, lon, h, &ok) > 0) a2 = m;
                        else b2 = m;
                    }
                    p.set = a2;
                }
                tt = tn;
                ee = en;
            }
            if (!p.set) p.set = tt;
            /* the top, by a golden search around the best sample */
            double lo = best_t - step, hi = best_t + step;
            for (int k = 0; k < 20; k++) {
                double m1 = hi - (hi - lo) * 0.618, m2 = lo + (hi - lo) * 0.618;
                if (el_at(s, m1, lat, lon, h, &ok) > el_at(s, m2, lat, lon, h, &ok)) hi = m2;
                else lo = m1;
            }
            p.top = 0.5 * (lo + hi);
            cl_look_t lk;
            if (cl_sat_look(s, p.top, lat, lon, h, std_mag, &lk)) {
                p.top_el = (float)lk.el;
                p.top_az = (float)lk.az;
            }
            if (cl_sat_look(s, p.rise, lat, lon, h, std_mag, &lk)) p.rise_az = (float)lk.az;
            if (cl_sat_look(s, p.set, lat, lon, h, std_mag, &lk)) p.set_az = (float)lk.az;
            /* the visible part, sampled every 10 s */
            p.mag = 99;
            const double vstep = 10.0 / 86400.0;
            for (double tv = p.rise; tv <= p.set; tv += vstep) {
                float mg;
                if (seen(s, tv, lat, lon, h, std_mag, &mg)) {
                    if (!p.vis_start) p.vis_start = tv;
                    p.vis_end = tv;
                    if (mg < p.mag) p.mag = mg;
                }
            }
            if (p.top_el >= el_min) out[n++] = p;
            t = p.set + step;
            e = el_at(s, t, lat, lon, h, &ok);
            if (!ok) break;
            continue;
        }
        t = t2;
        e = e2;
    }
    return n;
}
