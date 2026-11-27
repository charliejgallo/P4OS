/*
 * CIELO - double-precision maths of its own.
 *
 * The P4 has a single-precision FPU, and the firmware's symbol table lends
 * the apps libgcc's soft-float double arithmetic (__adddf3, __muldf3...)
 * but none of libm's double functions: sin(), atan2() or sqrt() would leave
 * the .so with a symbol the loader cannot resolve. Time is where a float
 * is not enough (a Julian date in a float is a quarter of a day), and SGP4
 * accumulates angles over thousands of minutes, so those two run in double
 * with these. Everything that only needs an angle already reduced to a
 * turn (the stars, the drawing) stays in float.
 *
 * The functions are plain series after a range reduction, good to about
 * 1e-15 relative; tests/test_astro.c checks them against the host's libm.
 */
#pragma once

#include <stdint.h>

#define CL_PI      3.14159265358979323846
#define CL_TWOPI   6.28318530717958647692
#define CL_D2R     (CL_PI / 180.0)
#define CL_R2D     (180.0 / CL_PI)

double cl_floor(double x);
/* x - y * floor(x / y), in [0, y) */
double cl_fmod_pos(double x, double y);
double cl_dabs(double x);
double cl_dsqrt(double x);
double cl_dcbrt(double x);
double cl_dsin(double x);
double cl_dcos(double x);
void   cl_dsincos(double x, double *s, double *c);
double cl_datan2(double y, double x);
double cl_dasin(double x);
double cl_dacos(double x);
/* x^n for a small whole n >= 0 */
double cl_dpowi(double x, int n);
