#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* /api/store/... (aos_portal_store.c); false when it is not this one */
bool aos_portal_store(aos_httpd_req_t *r, const char *method, const char *p);
