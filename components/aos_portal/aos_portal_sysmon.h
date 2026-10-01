/* P4OS - the portal's Monitor API (aos_portal_sysmon.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was a sysmon request (answered). */
bool aos_portal_sysmon(aos_httpd_req_t *r, const char *method, const char *p);
