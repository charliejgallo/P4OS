/* P4OS - the portal's Macro pad API (aos_portal_macropad.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was a Macro pad request (answered). */
bool aos_portal_macropad(aos_httpd_req_t *r, const char *method, const char *p);
