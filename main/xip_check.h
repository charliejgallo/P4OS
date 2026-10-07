/* Diagnostic, only with P4OS_XIP_CHECK in the environment at build time:
 * xip_check.c says why. */
#pragma once

void xip_check_run(void);   /* first thing in app_main */
void xip_check_log(void);   /* once the log ring is up */
