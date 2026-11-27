/*
 * CIELO - double-precision maths of its own (see cl_dmath.h).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cl_dmath.h"

#include <math.h>

/* pi/2 in two parts, so that x - k * pi/2 keeps its low bits for big k */
#define PIO2_HI 1.57079632673412561417e+00
#define PIO2_LO 6.07710050650619224932e-11

double cl_dabs(double x)
{
    return x < 0 ? -x : x;
}

double cl_floor(double x)
{
    /* the angles and dates here are far below 2^62 */
    int64_t i = (int64_t)x;
    double f = (double)i;
    return f > x ? f - 1.0 : f;
}

double cl_fmod_pos(double x, double y)
{
    double r = x - y * cl_floor(x / y);
    if (r < 0) r += y;
    if (r >= y) r -= y;
    return r;
}

double cl_dsqrt(double x)
{
    if (x <= 0) return 0;
    double y = (double)sqrtf((float)x);
    if (y == 0) y = 1e-150;
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return 0.5 * (y + x / y);
}

double cl_dcbrt(double x)
{
    if (x == 0) return 0;
    double ax = x < 0 ? -x : x;
    double y = (double)powf((float)ax, 1.0f / 3.0f);
    for (int i = 0; i < 3; i++) y = y - (y * y * y - ax) / (3.0 * y * y);
    return x < 0 ? -y : y;
}

/* sin and cos of |r| <= pi/4: Taylor to the 17th and 16th power, whose
 * first left-out terms are below 1e-17 there */
static double ksin(double r)
{
    double z = r * r;
    return r * (1.0 + z * (-1.0 / 6 + z * (1.0 / 120 + z * (-1.0 / 5040 + z * (1.0 / 362880 +
           z * (-1.0 / 39916800 + z * (1.0 / 6227020800.0 + z * (-1.0 / 1307674368000.0 +
           z * (1.0 / 355687428096000.0)))))))));
}

static double kcos(double r)
{
    double z = r * r;
    return 1.0 + z * (-0.5 + z * (1.0 / 24 + z * (-1.0 / 720 + z * (1.0 / 40320 + z * (-1.0 / 3628800 +
           z * (1.0 / 479001600.0 + z * (-1.0 / 87178291200.0 + z * (1.0 / 20922789888000.0))))))));
}

void cl_dsincos(double x, double *s, double *c)
{
    double kf = cl_floor(x / (CL_PI / 2) + 0.5);
    double r = (x - kf * PIO2_HI) - kf * PIO2_LO;
    int64_t k = (int64_t)kf;
    double sr = ksin(r), cr = kcos(r);
    switch ((int)(k & 3)) {
    case 0: *s = sr;  *c = cr;  break;
    case 1: *s = cr;  *c = -sr; break;
    case 2: *s = -sr; *c = -cr; break;
    default: *s = -cr; *c = sr; break;
    }
}

double cl_dsin(double x)
{
    double s, c;
    cl_dsincos(x, &s, &c);
    return s;
}

double cl_dcos(double x)
{
    double s, c;
    cl_dsincos(x, &s, &c);
    return c;
}

/* atan of 0 <= t <= 1: moved below tan(pi/12) with
 * atan t = pi/6 + atan((t sqrt3 - 1) / (t + sqrt3)), then the series */
static double katan(double t)
{
    static const double SQ3 = 1.73205080756887729353;
    double base = 0;
    if (t > 0.26794919243112270) {
        t = (t * SQ3 - 1.0) / (t + SQ3);
        base = CL_PI / 6;
    }
    double z = t * t, sum = 0;
    /* t^(2n+1) / (2n+1), down from n = 12: |t| <= 0.268 leaves < 1e-16 */
    for (int n = 12; n >= 0; n--) sum = ((n & 1) ? -1.0 : 1.0) / (double)(2 * n + 1) + z * sum;
    return base + t * sum;
}

double cl_datan2(double y, double x)
{
    double ax = cl_dabs(x), ay = cl_dabs(y);
    if (ax == 0 && ay == 0) return 0;
    double a = ay <= ax ? katan(ay / ax) : CL_PI / 2 - katan(ax / ay);
    if (x < 0) a = CL_PI - a;
    return y < 0 ? -a : a;
}

double cl_dasin(double x)
{
    if (x >= 1) return CL_PI / 2;
    if (x <= -1) return -CL_PI / 2;
    return cl_datan2(x, cl_dsqrt(1.0 - x * x));
}

double cl_dacos(double x)
{
    if (x >= 1) return 0;
    if (x <= -1) return CL_PI;
    return cl_datan2(cl_dsqrt(1.0 - x * x), x);
}

double cl_dpowi(double x, int n)
{
    double r = 1;
    while (n-- > 0) r *= x;
    return r;
}
