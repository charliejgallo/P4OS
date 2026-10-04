#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* Whether a request may go on to the portal (aos_portal_access.c). false: it
 * has been answered already (403, 421) and the handler stops there. */
bool aos_portal_access(aos_httpd_req_t *r);
