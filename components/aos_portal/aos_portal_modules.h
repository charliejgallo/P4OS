/* P4OS - the portal's Expansión API (aos_portal_modules.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was an expansion or sensors
 * request (answered). */
bool aos_portal_modules(aos_httpd_req_t *r, const char *method, const char *p);
