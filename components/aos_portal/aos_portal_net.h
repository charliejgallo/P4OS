#pragma once
#include <stdbool.h>
#include "aos_httpd.h"

/* GET/POST /api/net (aos_portal_net.c); false when it is not this one */
bool aos_portal_net(aos_httpd_req_t *r, const char *method, const char *p);
