#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* GET/POST /api/radio (aos_portal_radio.c); false when it is not this one */
bool aos_portal_radio(aos_httpd_req_t *r, const char *method, const char *p);
