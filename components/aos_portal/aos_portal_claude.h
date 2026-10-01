/* P4OS - the portal's Claude login and status (aos_portal_claude.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was a Claude request (answered). */
bool aos_portal_claude(aos_httpd_req_t *r, const char *method, const char *p);
