/* P4OS - the portal's Ethernet API (aos_portal_eth.c). */
#pragma once

#include <stdbool.h>

#include "aos_httpd.h"

bool aos_portal_eth(aos_httpd_req_t *r, const char *method, const char *p);
