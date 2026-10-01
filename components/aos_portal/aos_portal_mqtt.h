/* P4OS - the portal's MQTT API (aos_portal_mqtt.c). */
#pragma once

#include "aos_httpd.h"
#include <stdbool.h>

/* p is the path after "/api/"; true when it was an MQTT request (answered). */
bool aos_portal_mqtt(aos_httpd_req_t *r, const char *method, const char *p);
