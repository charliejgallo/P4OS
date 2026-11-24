#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* /api/154... (aos_portal_154.c); false when it is not one of them */
bool aos_portal_154(aos_httpd_req_t *r, const char *method, const char *p);
