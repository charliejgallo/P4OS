/* P4OS - the portal's Banco API (aos_portal_bench.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was a bench request (answered). */
bool aos_portal_bench(aos_httpd_req_t *r, const char *method, const char *p);
