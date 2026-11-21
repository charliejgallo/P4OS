#pragma once

#include <stdbool.h>
#include "aos_httpd.h"

/* /api/backup/... (aos_portal_backup.c); false when it is not this one */
bool aos_portal_backup(aos_httpd_req_t *r, const char *method, const char *p);
