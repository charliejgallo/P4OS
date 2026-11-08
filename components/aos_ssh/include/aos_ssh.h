/*
 * P4OS - the SSH server (components/aos_ssh, docs/SSH.md): a console on the
 * board from any ssh client. Start it once at boot; it listens only while
 * Settings, Portal web has it on, and follows the portal's rules
 * (aos_access.h).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool aos_ssh_start(int port);
int  aos_ssh_sessions(void);        /* logged in now */

#ifdef __cplusplus
}
#endif
