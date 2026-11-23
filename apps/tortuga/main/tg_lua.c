/*
 * Tortuga - Lua inside the game. Lua 5.4.8 (../lua, the same tree as the
 * Lua app's) is compiled here as one unit with every function static, the
 * way Lua's own onelua.c does: nothing of it is exported, so it cannot
 * clash with the Lua app when the simulator links every app together, and
 * the tree is outside main/ so the board's build does not compile its files
 * one by one. Only base, math, string, table and utf8 are opened.
 *
 * A program runs in one fresh state per map: the turtle's orders are C
 * functions that call the world, the line comes from the caller's frame,
 * and a count hook stops a program that never ends.
 */
#include "tg_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* ----------------------------------------------------------- Lua, static */

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

/* lua_ident is the one global LUA_API cannot make static (lua.h declares it
 * extern): renamed, or it collides with the Lua app's in the simulator, where
 * every app is linked into one binary */
#define lua_ident tg_lua_ident
#define LUA_32BITS 1
#define LUA_CORE
#define LUA_LIB
#include "../lua/luaconf.h"
#undef LUAI_FUNC
#undef LUAI_DDEC
#undef LUAI_DDEF
#undef LUA_API
#define LUAI_FUNC       static
#define LUAI_DDEC(def)  /* empty */
#define LUAI_DDEF       static
#define LUA_API         static

#include "../lua/lzio.c"
#include "../lua/lctype.c"
#include "../lua/lopcodes.c"
#include "../lua/lmem.c"
#include "../lua/lundump.c"
#include "../lua/ldump.c"
#include "../lua/lstate.c"
#include "../lua/lgc.c"
#include "../lua/llex.c"
#include "../lua/lcode.c"
#include "../lua/lparser.c"
#include "../lua/ldebug.c"
#include "../lua/lfunc.c"
#include "../lua/lobject.c"
#include "../lua/ltm.c"
#include "../lua/lstring.c"
#include "../lua/ltable.c"
#include "../lua/ldo.c"
#include "../lua/lvm.c"
#include "../lua/lapi.c"
#include "../lua/lauxlib.c"
#include "../lua/lbaselib.c"
#include "../lua/lmathlib.c"
#include "../lua/lstrlib.c"
#include "../lua/ltablib.c"
#include "../lua/lutf8lib.c"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "tg_lua.h"

#define HEAP_MAX    (4u * 1024 * 1024)
#define HOOK_EVERY  1000
#define HOOK_MAX    3000        /* 3 million instructions */

typedef struct {
    tg_world_t *w;
    size_t heap;
    int hooks;
    bool stopped;               /* the world stopped the program */
} run_t;

static void *lalloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    run_t *r = (run_t *)ud;
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
        free(ptr);
#else
        heap_caps_free(ptr);
#endif
        r->heap -= old;
        return NULL;
    }
    if (nsize > old && r->heap + (nsize - old) > HEAP_MAX) return NULL;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    void *p = realloc(ptr, nsize);
#else
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p) r->heap = r->heap - old + nsize;
    return p;
}

static run_t *run_of(lua_State *L)
{
    void *ud;
    lua_getallocf(L, &ud);
    return (run_t *)ud;
}

static int line_here(lua_State *L)
{
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "l", &ar)) return ar.currentline;
    return 0;
}

static void hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    run_t *r = run_of(L);
    if (++r->hooks > HOOK_MAX) {
        tg_world_fail(r->w, TG_F_LOOP, line_here(L), NULL);
        r->stopped = true;
        lua_pushstring(L, "");
        lua_error(L);
    }
}

static int order(lua_State *L)
{
    run_t *r = run_of(L);
    int o = (int)lua_tointeger(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    float num[2] = { 0 };
    const char *str[2] = { 0 };
    int line = line_here(L);
    for (int i = 0; i < n && i < 2; i++) {
        int t = lua_type(L, i + 1);
        if (t == LUA_TNUMBER) {
            num[i] = (float)lua_tonumber(L, i + 1);
        } else if (t == LUA_TSTRING) {
            str[i] = lua_tostring(L, i + 1);
        } else {
            char m[96];
            snprintf(m, sizeof m, _("«%s» lleva un número"), tg_orders[o].es);
            tg_world_fail(r->w, TG_F_RUNTIME, line, m);
            r->stopped = true;
            lua_pushstring(L, "");
            return lua_error(L);
        }
    }
    bool out = false;
    if (!tg_world_call(r->w, o, num, str, n > 2 ? 3 : n, line, &out)) {
        r->stopped = true;
        lua_pushstring(L, "");
        return lua_error(L);
    }
    if (tg_orders[o].sense) {
        lua_pushboolean(L, out);
        return 1;
    }
    return 0;
}

/* Lua's message, in the player's words where we know it: "programa:3:
 * attempt to call a nil value (global 'avanzr')" */
static void explain(const char *msg, int *line, char *out, size_t n)
{
    *line = 0;
    const char *p = strstr(msg, "programa:");
    if (p) {
        p += 9;
        *line = atoi(p);
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p == ' ') p++;
    } else {
        p = msg;
    }
    const char *q;
    char name[40] = "";
    if ((q = strstr(p, "(global '")) != NULL) {
        sscanf(q + 9, "%39[^']", name);
        if (strstr(p, "attempt to call")) {
            snprintf(out, n, _("no conozco «%s»"), name);
            return;
        }
        if (strstr(p, "arithmetic") || strstr(p, "compare")) {
            snprintf(out, n, _("«%s» no tiene valor todavía"), name);
            return;
        }
    }
    if ((q = strstr(p, "'end' expected")) != NULL) {
        int at = 0;
        const char *l = strstr(q, "at line ");
        if (l) at = atoi(l + 8);
        if (at) snprintf(out, n, _("falta un «end» para cerrar lo que abre la línea %d"), at);
        else snprintf(out, n, "%s", _("falta un «end»"));
        return;
    }
    if (strstr(p, "'then' expected")) {
        snprintf(out, n, "%s", _("falta el «then» del «if»"));
        return;
    }
    if (strstr(p, "'do' expected")) {
        snprintf(out, n, "%s", _("falta el «do»"));
        return;
    }
    if (strstr(p, "')' expected")) {
        snprintf(out, n, "%s", _("falta cerrar el paréntesis"));
        return;
    }
    if (strstr(p, "not enough memory")) {
        snprintf(out, n, "%s", _("el programa usó demasiada memoria"));
        return;
    }
    snprintf(out, n, "%s", p);
}

void tg_lua_run(const char *src, tg_world_t *w)
{
    run_t r = { w, 0, 0, false };
    lua_State *L = lua_newstate(lalloc, &r);
    if (!L) {
        tg_world_fail(w, TG_F_RUNTIME, 0, _("no hay memoria"));
        return;
    }
    static const luaL_Reg LIBS[] = {
        { LUA_GNAME, luaopen_base }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_STRLIBNAME, luaopen_string },
        { LUA_TABLIBNAME, luaopen_table }, { LUA_UTF8LIBNAME, luaopen_utf8 },
    };
    for (size_t i = 0; i < sizeof(LIBS) / sizeof(LIBS[0]); i++) {
        luaL_requiref(L, LIBS[i].name, LIBS[i].func, 1);
        lua_pop(L, 1);
    }
    /* no files from a program */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    for (int o = 0; o < TG_O_N; o++) {
        lua_pushinteger(L, o);
        lua_pushcclosure(L, order, 1);
        lua_pushvalue(L, -1);
        lua_setglobal(L, tg_orders[o].es);
        lua_setglobal(L, tg_orders[o].en);
    }
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    int rc = luaL_loadbufferx(L, src, strlen(src), "=programa", "t");
    if (rc != LUA_OK) {
        char m[120];
        int line;
        explain(lua_tostring(L, -1), &line, m, sizeof m);
        tg_world_fail(w, TG_F_SYNTAX, line, m);
    } else {
        rc = lua_pcall(L, 0, 0, 0);
        if (rc != LUA_OK && !r.stopped && w->state == TG_ST_RUN) {
            char m[120];
            int line;
            const char *e = lua_tostring(L, -1);
            explain(e ? e : "?", &line, m, sizeof m);
            tg_world_fail(w, TG_F_RUNTIME, line, m);
        }
    }
    lua_close(L);
    tg_world_finish(w);
}
