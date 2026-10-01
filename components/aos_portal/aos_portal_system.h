/*
 * P4OS - the portal's system endpoints: firmware updates over Wi-Fi and the
 * core dump a panic left (aos_portal_system.c).
 */
#pragma once

#include <stdbool.h>
#include "aos_httpd.h"

/* true if it answered the request */
bool aos_portal_system(aos_httpd_req_t *r, const char *method, const char *path);
