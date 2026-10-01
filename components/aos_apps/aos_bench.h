/*
 * P4OS - the Banco (bench) app: the pieces its shell (aos_app_bench.c) puts
 * in its tabs.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Osciloscopio (aos_bench_scope.c) ------------------------------------
 * The Rigol DS1000Z over aos_scope.h. build() fills 'parent', the tab's
 * content area of w x h, and turns the traces on; tick() is called about
 * every 100 ms while the tab shows; destroy() stops the traces and forgets
 * its objects without deleting any (the parent's owner does that); back()
 * closes an overlay of its own (the scope's screenshot, the keyboard) and
 * says true, or false when there is nothing to close. */
void bench_scope_build(lv_obj_t *parent, int32_t w, int32_t h, bool landscape);
void bench_scope_tick(void);
void bench_scope_destroy(void);
bool bench_scope_back(void);
/* ---- end of Osciloscopio ------------------------------------------------- */

#ifdef __cplusplus
}
#endif
