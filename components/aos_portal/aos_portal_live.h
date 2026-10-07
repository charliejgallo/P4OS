#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* GET/POST /api/live (aos_portal_live.c); false when it is not this one */
bool aos_portal_live(aos_httpd_req_t *r, const char *method, const char *p);
