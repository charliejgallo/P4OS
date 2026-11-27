/* P4OS - the portal's page for the Telegram bot (aos_portal_bot.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was a bot request (answered). */
bool aos_portal_bot(aos_httpd_req_t *r, const char *method, const char *p);
