/*
 * P4OS - what librtlsdr's sources see of the system (apps/rf/README.md).
 *
 * Included by each vendored file after its own system headers: its messages
 * go to the board's log (the portal's Registro, tag "rf") instead of a
 * stderr nobody reads, and its waits through the HAL.
 */
#ifndef RF_PORT_H
#define RF_PORT_H

#include <stdio.h>

void rf_port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rf_port_usleep(unsigned us);

#undef fprintf
#define fprintf(stream, ...) rf_port_log(__VA_ARGS__)
#undef usleep
#define usleep(us) rf_port_usleep(us)

#endif
