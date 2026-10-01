/*
 * P4OS - Opening an app on something: aos_ui_open_app_with() (aos_ui.h).
 *
 * One argument waits at a time, for one app id, only for the length of the
 * aos_ui_open() it goes with: that call builds and shows the app before it
 * returns, and the app takes the argument from its show(). Whatever it did
 * not take is forgotten right after, so a plain open later never finds a
 * stale path waiting.
 */
#include "aos_ui.h"
#include "aos_hal.h"

#include <string.h>

#define ARG_LEN 512

static char s_id[64];
static char s_arg[ARG_LEN];
static char s_taken[ARG_LEN];
static bool s_pending;

bool aos_ui_open_app_with(const char *id, const char *arg)
{
    if (!id) return false;
    size_t li = strlen(id), la = arg ? strlen(arg) : 0;
    if (li >= sizeof s_id || la >= sizeof s_arg) {
        aos_hal_log("ui", "open_app_with: too long for %s", id);
        return false;
    }
    memcpy(s_id, id, li + 1);
    if (arg) memcpy(s_arg, arg, la + 1);
    else s_arg[0] = 0;
    s_pending = true;
    bool ok = aos_ui_open(id);
    s_pending = false;
    return ok;
}

const char *aos_ui_take_open_arg(const char *id)
{
    if (!s_pending || !id || strcmp(id, s_id)) return NULL;
    s_pending = false;
    memcpy(s_taken, s_arg, sizeof s_taken);
    return s_taken;
}
