#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* Whether a request may go on to the portal (aos_portal_access.c). false: it
 * has been answered already (403, 421) and the handler stops there. */
bool aos_portal_access(aos_httpd_req_t *r);

/* Then the rules (aos_access.h): closed on this network, a login, or open.
 * It also answers /api/auth, /api/login and /api/logout itself. false: it
 * answered. */
bool aos_portal_rules(aos_httpd_req_t *r);
