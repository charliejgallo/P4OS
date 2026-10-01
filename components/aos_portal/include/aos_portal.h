/*
 * P4OS - the web portal (aos_portal.c): the board from a browser. Start it
 * once at boot; it serves as soon as there is a network.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool aos_portal_start(int port);

#ifdef __cplusplus
}
#endif
