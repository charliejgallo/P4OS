/*
 * LUA - scripts on the card, on the whole screen
 *
 * Two ways in, the same script either way: a list of the .lua files in
 * /sdcard/lua, and -because this module declares one app per script- an entry
 * of its own in the launcher, with its name, its colour and its icon, beside
 * the apps written in C. The interpreter is paid for once; every script after
 * that is data.
 *
 * A script says what it wants to happen by defining functions:
 *
 *     function init()            once, before the first frame
 *     function tick(dt)          every frame, dt in milliseconds
 *     function draw()            every frame, after tick
 *     function touch(x, y, ev)   ev is "down", "move" or "up"
 *     function gesture(ev, x, y, a, b, c)   "tap", "double", "long", "drag"
 *                                (a,b = dx,dy), "release" (a,b = speed per
 *                                second), "pinchstart", "pinch" (a = scale,
 *                                b,c = dx,dy), "pinchend"
 *     function resize(w, h)      P4OS: the screen turned and the canvas with
 *                                it; aos.W and aos.H are already the new ones
 *
 * All of them are optional. Everything a script can reach is the 'aos' table
 * (lx_api.c); there is no io, no os and no package, because their sources are
 * not compiled into the binary.
 *
 * The promise this app makes, and the reason it exists: a broken script is a
 * message with a line number, never a reboot. Four things hold it up:
 *
 *   - Every call into Lua goes through lua_pcall.
 *   - A count hook cuts a script that will not come back, and once it has cut
 *     one it fires on every instruction, so a pcall inside the script cannot
 *     swallow the cut and carry on.
 *   - The C recursion is bounded by LUAI_MAXCCALLS, set in the CMakeLists with
 *     the measurement that explains the number.
 *   - P4OS: the script runs on a thread of its own, with a 96 KB stack in
 *     PSRAM, and not in the LVGL task. That task has 12 KB here, and a nested
 *     pcall costs 700 bytes of it (CMakeLists.txt): no guard could make it
 *     safe. On its own thread the interface never waits for a script either;
 *     a slow one only gets fewer frames.
 *
 * Drawing (P4OS): the script draws into a buffer of its own, and the app
 * copies what changed onto the OS's retro canvas (aos_retro.h, docs/RETRO.md),
 * which the OS scales onto the screen. By default the canvas fills the
 * screen at x3: 240x426 upright, 426x240 lying down. A script may ask for
 * another size in its first lines:
 *
 *     -- @canvas 184x224          the watch's canvas, x3 and centred
 *     -- @canvas 360x640          x2
 *     -- @orientation portrait    or landscape: the screen turns for it
 *                                 (launcher entries only)
 *
 * Input is the finger -touch(), gesture(), aos.touch() and aos.fingers()-
 * and, since 0.9, a USB gamepad: aos.pad(). The board has no buttons and no
 * motion sensor. The way back is the system's: the left edge (back to the
 * list, or out) and the bottom edge (home). The pad also goes down the list
 * of scripts (aos_pad_menu) and A on an error message reloads the script.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad_menu.h"
#include "aos_retro.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_gesture.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lx_api.h"
#include "lx_pixel.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef AOS_SIM
#include "esp_heap_caps.h"
#endif

#define LUA_MAX_SCRIPTS     256

/* How many scripts also become apps of their own in the launcher.
 *
 * Lower than LUA_MAX_SCRIPTS on purpose: the list inside this app can show
 * everything on the card, but every launcher entry takes a slot of the
 * firmware's MAX_DYNAPPS (224 on P4OS), which the .so files share. 192 leaves
 * room for 32 .so files next to a card full of scripts; the loader says which
 * script did not fit. A script past the last still runs: it is in the list,
 * it just does not get its own icon. */
#define LUA_MAX_APPS        192
#define LUA_MAX_SOURCE      (128 * 1024)    /* a script bigger than this is
                                             * not a script, it is a mistake */
#define LUA_HEAD            512             /* where the @ lines are looked for */
#define LUA_FPS             50              /* the frame's ceiling; a frame
                                             * takes what it takes */
#define LUA_BUDGET_MS       400             /* per call into Lua, before the
                                             * hook cuts it */
#define LUA_LOAD_MS         2000            /* the chunk and init(): a script
                                             * may build its world there */
#define LUA_HOOK_COUNT      2000            /* instructions between checks:
                                             * well under a millisecond */
#define LUA_HEAP_MAX        (4u * 1024 * 1024)  /* what one script may take of
                                             * the 32 MB of PSRAM; past it
                                             * Lua says "not enough memory" */
#define LUA_STACK           (96 * 1024)     /* the thread's, in PSRAM; see
                                             * LUAI_MAXCCALLS in CMakeLists */
#define LUA_PRIO            3               /* level with the apps' threads,
                                             * below LVGL's 4 */
#define LUA_MAX_EVENTS      32
#define LUA_EDGE_BOTTOM     36              /* the home swipe's strip */

/* The canvas limits a script may ask for. The top is the screen itself at
 * x1; the bottom keeps a canvas from being a smear of 80-pixel squares. */
#define LUA_MIN_SIDE        16
#define LUA_MAX_SIDE        1280
#define LUA_MAX_PX          (720 * 1280)
#define LUA_FILL_DIV        3               /* the default canvas: the screen
                                             * over this, x this             */

/* What a script says about itself in its first lines. */
typedef struct {
    char     name[48];
    int16_t  w, h;              /* the canvas it asked for; 0 = fill       */
    uint32_t orient;            /* AOS_APP_FLAG_PORTRAIT / _LANDSCAPE or 0 */
} lua_head_t;

/* A finger event on its way to the script. The LVGL task queues them and the
 * script's thread gets them at the start of its next frame, before tick(). */
enum { EV_DOWN = 0, EV_MOVE, EV_UP, EV_GESTURE };

typedef struct {
    uint8_t     kind;           /* EV_*                                     */
    uint8_t     nargs;          /* gesture: how many of a, b, c go          */
    const char *name;           /* gesture: "tap", "pinch"...               */
    int16_t     x, y;
    float       a, b, c;
} lua_ev_t;

/* The handoff between the LVGL task and the script's thread.
 *
 * One word says who owns everything shared: while 'phase' is PH_IDLE it is
 * the LVGL task's, while it is PH_BUSY it is the thread's. The LVGL task
 * fills in a request and sets BUSY; the thread does it and sets IDLE. Nobody
 * ever waits for the other in the normal course: the LVGL task only looks at
 * the word on its frame timer and moves on if the thread is still busy. That
 * one word, with acquire and release, is the whole of the locking. */
enum { PH_IDLE = 0, PH_BUSY };
enum { REQ_NONE = 0, REQ_LOAD, REQ_FRAME, REQ_RESIZE, REQ_QUIT };

typedef struct {
    lv_obj_t  *root;
    lv_obj_t  *list;            /* the screen with one button per script    */
    lv_obj_t  *surface;         /* transparent, exactly over the canvas: the
                                 * finger, the gestures                      */
    lv_obj_t  *message;         /* the error, over the canvas               */
    aos_app_t *self;
    bool       standalone;      /* opened as its own app, not from the list */

    /* --- the canvas: the LVGL task's --- */
    const aos_retro_t *r;
    lua_head_t head;            /* of the running script                    */

    /* --- owned by whoever 'phase' says --- */
    uint16_t  *work;            /* w x h, what the script draws on          */
    lx_buf_t   buf;
    lx_ctx_t   api;

    /* Two lists, and the difference matters.
     *
     * 'drawn' is what the SCRIPT touched: the primitives mark it, and it is
     * what has to be undone next frame. 'dirty' is what goes to the screen,
     * which is that plus whatever was undone at the start of this one.
     *
     * With one list the undo marks fed back into themselves: the first frame
     * is a whole screen -init() clears- so the second undid the whole screen
     * and marked it, and it stayed at every row for ever. */
    lx_dirty_t drawn;
    lx_dirty_t dirty;

    /* The frozen background and what was drawn last time. With a background,
     * the app undoes the previous frame at the start of this one instead of
     * making the script erase: aos.background() in lx_api.c says why. */
    uint16_t  *back;
    lx_dirty_t prev;

    int        phase;           /* PH_*, read and written atomically        */
    int        req;             /* REQ_*, for the thread                    */
    int        cancel;          /* the LVGL task wants the thread back now  */
    int        exited;          /* the thread returned for good             */
    bool       fresh;           /* a request came back and is not harvested */
    uint32_t   frame_dt;
    lua_ev_t   ev[LUA_MAX_EVENTS];
    int        n_ev;

    /* What the thread leaves for the LVGL task. */
    bool       stopped;         /* an error already killed this script      */
    bool       err_new;
    char       err[400];
    bool       has_touch, has_gesture, has_resize;

    /* The Lua state: the thread's only. */
    lua_State *L;
    int        ref_tick;
    int        ref_draw;
    int        ref_touch;
    int        ref_gesture;
    int        ref_resize;
    uint32_t   call_start;      /* for the hook: when this call began       */
    uint32_t   call_budget;
    size_t     heap_used;

    /* --- the LVGL task's only --- */
    bool       thread_on;
    lua_ev_t   pend[LUA_MAX_EVENTS];    /* waiting for the next frame       */
    int        n_pend;
    int16_t    t_x, t_y;
    bool       t_down;
    /* The USB gamepad: read by pad_cb() every 20 ms whatever is on screen.
     * Over a script, the presses gather in pad_pend until pump() hands them
     * over with the next frame; over the list, they drive pad_menu. */
    aos_pad_t      pad;
    uint16_t       pad_pend;
    aos_pad_menu_t pad_menu;
    lv_timer_t    *pad_timer;
    bool       want_touch, want_gesture;
    uint32_t   last_post;
    bool       want_reload;

    char       names[LUA_MAX_SCRIPTS][48];
    int        count;
    int        pick;            /* the list's button that was tapped        */

    /* The script that is running, and what it looked like on disk when it
     * was loaded: the app watches those two numbers and reloads itself when
     * the file changes. That is what makes saving the only step -write it on
     * the Mac, look at the board- instead of save, walk over, back out, tap
     * again. */
    char       running[48];
    uint32_t   watch_at;
    time_t     watch_mtime;
    long       watch_size;
} lua_ctx_t;

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static int ph_get(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void ph_set(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* ==========================================================================
 * What a script says about itself
 * ========================================================================== */

/* The value after "@tag", up to the end of its line, trimmed. */
static bool head_tag(const char *text, const char *tag, char *out, size_t out_len)
{
    const char *p = strstr(text, tag);
    if (!p) {
        return false;
    }
    p += strlen(tag);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n < out_len - 1) n++;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n == 0) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* The name shown in the launcher, the canvas and the orientation. A script
 * says them with comments on any of its first lines:
 *
 *     -- @name Cubo giratorio
 *     -- @canvas 184x224
 *     -- @orientation portrait
 *
 * Without @name the file name, minus the extension, is used. It is worth the
 * lines: otherwise every entry in the launcher is a lower-case file name
 * among apps that are called Burbujas and Pixel Art. A canvas that makes no
 * sense is ignored rather than refused: the script still runs, on the
 * default one. */
static void head_read(const char *file, lua_head_t *h)
{
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "%s", file);
    char *dot = strrchr(h->name, '.');
    if (dot) {
        *dot = '\0';
    }

    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/lua/%s", sd, file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    char text[LUA_HEAD];
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';

    char v[48];
    if (head_tag(text, "@name", v, sizeof(v))) {
        snprintf(h->name, sizeof(h->name), "%s", v);
    }
    if (head_tag(text, "@canvas", v, sizeof(v))) {
        int w = 0, hh = 0;
        if (sscanf(v, "%dx%d", &w, &hh) == 2 &&
            w >= LUA_MIN_SIDE && hh >= LUA_MIN_SIDE &&
            w <= LUA_MAX_SIDE && hh <= LUA_MAX_SIDE && w * hh <= LUA_MAX_PX) {
            h->w = (int16_t)w;
            h->h = (int16_t)hh;
        }
    }
    if (head_tag(text, "@orientation", v, sizeof(v))) {
        if (strcasecmp(v, "portrait") == 0) {
            h->orient = AOS_APP_FLAG_PORTRAIT;
        } else if (strcasecmp(v, "landscape") == 0) {
            h->orient = AOS_APP_FLAG_LANDSCAPE;
        }
    }
}

/* ==========================================================================
 * The script's side: all of it runs on the script's thread
 * ========================================================================== */

/* Lua's heap, in PSRAM, and with a ceiling.
 *
 * Without this, lua_newstate would use realloc, and realloc obeys
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024: Lua allocates in crumbs, so every
 * one of them would land in the scarce internal RAM (docs/MEMORY.md). On the
 * watch a state grown to 77 KB took the free internal RAM from 107 K to
 * 56.8 K with the default allocator, and 52 bytes with this one, at 2 % of
 * speed.
 *
 * The ceiling is new: 32 MB of PSRAM are there for a script to take, and
 * 'local t = {} for i = 1, 1e9 do t[i] = i end' would take them from the
 * whole system. Past LUA_HEAP_MAX this returns NULL, Lua collects and tries
 * again, and then the script gets "not enough memory" like any other error.
 *
 * 'ud' is the app's context: the hook finds it here too (lua_getallocf). */
static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lua_ctx_t *ctx = (lua_ctx_t *)ud;
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
#ifdef AOS_SIM
        free(ptr);
#else
        heap_caps_free(ptr);
#endif
        ctx->heap_used -= old;
        return NULL;
    }
    if (nsize > old && ctx->heap_used + (nsize - old) > LUA_HEAP_MAX) {
        return NULL;
    }
#ifdef AOS_SIM
    void *p = realloc(ptr, nsize);
#else
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
#endif
    if (p) {
        ctx->heap_used = ctx->heap_used - old + nsize;
    }
    return p;
}

/* The hook that keeps a runaway script from running for ever. It is set on
 * LUA_MASKCOUNT, so it fires every LUA_HOOK_COUNT instructions no matter what
 * the script is doing -a bare 'while true do end' included.
 *
 * Once it cuts, it re-arms itself for EVERY instruction. The error it raises
 * is an ordinary Lua error, and a script that wraps its loop in pcall would
 * catch it and go round again; with the hook on every instruction the first
 * instruction outside that pcall raises it once more, and so on outwards
 * until it reaches ours. call_lua() puts the count back for the next call.
 *
 * It also answers the LVGL task: 'cancel' is how leaving the app gets the
 * thread back within an instruction count instead of a frame. */
static void budget_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    void *ud = NULL;
    lua_getallocf(L, &ud);
    lua_ctx_t *ctx = (lua_ctx_t *)ud;
    if (!ctx) {
        return;
    }
    bool cancel = ph_get(&ctx->cancel) != 0;
    if (cancel || now_ms() - ctx->call_start > ctx->call_budget) {
        lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
        if (cancel) {
            luaL_error(L, "stopped");
        }
        luaL_error(L, "the script did not return in %d ms", (int)ctx->call_budget);
    }
}

/* What a console can read: one file with the running script on the first
 * line and its error, if any, on the rest.
 *
 * A file and not an endpoint, on purpose: on the watch the portal's /lua page
 * read it with the same download that serves the scripts, and the firmware
 * still knew nothing about Lua -the same arrangement as the .pato scripts and
 * the .pix drawings. The name starts with an underscore and not a dot
 * because the portal refuses dot-files, and it does not end in .lua so
 * neither this app's list nor a browser's shows it as a script. */
static void write_state(lua_ctx_t *ctx, const char *error)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/lua/_estado.txt", sd);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n%s\n", ctx->running, error ? error : "");
    fclose(f);
}

/* The script is dead: its message is left for the LVGL task to show. */
static void fail(lua_ctx_t *ctx, const char *what)
{
    snprintf(ctx->err, sizeof(ctx->err), "%s", what ? what : "error with no message");
    ctx->err_new = true;
    ctx->stopped = true;
    write_state(ctx, ctx->err);
}

/* Every call into Lua goes through here: the budget is reset, the error is
 * caught, and the script is stopped with its message. */
static bool call_lua(lua_ctx_t *ctx, int nargs, uint32_t budget_ms)
{
    ctx->call_start = now_ms();
    ctx->call_budget = budget_ms;
    lua_sethook(ctx->L, budget_hook, LUA_MASKCOUNT, LUA_HOOK_COUNT);
    if (lua_pcall(ctx->L, nargs, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(ctx->L, -1);
        fail(ctx, msg);
        lua_pop(ctx->L, 1);
        return false;
    }
    return true;
}

/* Picks up a global if it is a function, and keeps it in the registry so the
 * frame does not go through a global lookup -or through whatever the script
 * may have done to the globals table since. */
static int grab(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void state_close(lua_ctx_t *ctx)
{
    if (ctx->L) {
        lua_close(ctx->L);
        ctx->L = NULL;
    }
    ctx->ref_tick = ctx->ref_draw = ctx->ref_touch = LUA_NOREF;
    ctx->ref_gesture = ctx->ref_resize = LUA_NOREF;
    ctx->has_touch = ctx->has_gesture = ctx->has_resize = false;
}

/* aos.background(): the copy is made here, where the buffers live. Allocated
 * the first time it is asked for, so a script that never calls this costs
 * nothing; called again, it re-freezes, which is how a script changes its
 * world between levels. */
static bool freeze_background(void *arg)
{
    lua_ctx_t *ctx = (lua_ctx_t *)arg;
    size_t bytes = (size_t)ctx->api.w * ctx->api.h * 2;
    if (!ctx->back) {
        ctx->back = (uint16_t *)malloc(bytes);
        if (!ctx->back) {
            return false;
        }
    }
    memcpy(ctx->back, ctx->work, bytes);
    lx_dirty_reset(&ctx->prev);     /* nothing of the old world to undo */
    return true;
}

/* REQ_LOAD: read the file, make a state, run the chunk and init(). The canvas
 * is already the size the script asked for: the LVGL task made it from the
 * same file's head before asking. */
static void do_load(lua_ctx_t *ctx)
{
    state_close(ctx);
    ctx->stopped = false;
    ctx->err_new = false;

    char path[160];
    const char *sd = aos_hal_path_sd_root();
    snprintf(path, sizeof(path), "%s/lua/%s", sd ? sd : "/sdcard", ctx->running);

    /* Read it whole and hand it to luaL_loadbuffer instead of using
     * luaL_loadfile: the file is small, this way the chunk name is ours, and
     * the reader never sits inside the parser holding the card open. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail(ctx, path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0 || len > LUA_MAX_SOURCE) {
        fclose(f);
        fail(ctx, "the file is empty or too big");
        return;
    }
    char *src = (char *)malloc((size_t)len + 1);
    if (!src) {
        fclose(f);
        fail(ctx, "no memory for the script");
        return;
    }
    size_t got = fread(src, 1, (size_t)len, f);
    fclose(f);
    src[got] = 0;

    ctx->heap_used = 0;
    ctx->L = lua_newstate(lua_alloc, ctx);
    if (!ctx->L) {
        free(src);
        fail(ctx, "no memory for the interpreter");
        return;
    }
    luaL_openlibs(ctx->L);
    ctx->api.t0 = lv_tick_get();
    lx_api_open(ctx->L, &ctx->api);

    char chunk[64];
    snprintf(chunk, sizeof(chunk), "@%s", ctx->running);
    int rc = luaL_loadbuffer(ctx->L, src, got, chunk);
    free(src);
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(ctx->L, -1);
        fail(ctx, msg ? msg : "it does not compile");
        return;
    }
    if (!call_lua(ctx, 0, LUA_LOAD_MS)) {
        return;                     /* the chunk itself blew up */
    }

    ctx->ref_tick    = grab(ctx->L, "tick");
    ctx->ref_draw    = grab(ctx->L, "draw");
    ctx->ref_touch   = grab(ctx->L, "touch");
    ctx->ref_gesture = grab(ctx->L, "gesture");
    ctx->ref_resize  = grab(ctx->L, "resize");
    ctx->has_touch   = ctx->ref_touch != LUA_NOREF;
    ctx->has_gesture = ctx->ref_gesture != LUA_NOREF;
    ctx->has_resize  = ctx->ref_resize != LUA_NOREF;

    lua_getglobal(ctx->L, "init");
    if (lua_isfunction(ctx->L, -1)) {
        if (!call_lua(ctx, 0, LUA_LOAD_MS)) return;
    } else {
        lua_pop(ctx->L, 1);
    }

    /* The first frame shows everything, and 'drawn' covers it too, so that
     * a script that draws in init() and never again still gets it undone
     * once the background is frozen. */
    lx_dirty_all(&ctx->dirty);
    lx_dirty_all(&ctx->drawn);
    write_state(ctx, NULL);         /* it loaded: the console goes quiet */
}

static void deliver(lua_ctx_t *ctx, const lua_ev_t *e)
{
    lua_State *L = ctx->L;
    if (e->kind == EV_GESTURE) {
        if (ctx->ref_gesture == LUA_NOREF) return;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ref_gesture);
        lua_pushstring(L, e->name);
        lua_pushinteger(L, e->x);
        lua_pushinteger(L, e->y);
        if (e->nargs > 0) lua_pushnumber(L, e->a);
        if (e->nargs > 1) lua_pushnumber(L, e->b);
        if (e->nargs > 2) lua_pushnumber(L, e->c);
        call_lua(ctx, 3 + e->nargs, LUA_BUDGET_MS);
        return;
    }
    if (ctx->ref_touch == LUA_NOREF) return;
    /* "down", "move" or "up", and not a boolean: a script told only "the
     * finger is down" cannot tell one tap from holding still -the first
     * cubo.lua added a cube per frame and the count wrapped round three
     * times during a single touch. */
    static const char *const EV[] = { "down", "move", "up" };
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ref_touch);
    lua_pushinteger(L, e->x);
    lua_pushinteger(L, e->y);
    lua_pushstring(L, EV[e->kind]);
    call_lua(ctx, 3, LUA_BUDGET_MS);
}

/* REQ_FRAME: undo, the finger, tick(), draw(). What changed is left in
 * 'dirty' for the LVGL task to copy onto the canvas. */
static void do_frame(lua_ctx_t *ctx)
{
    if (!ctx->L || ctx->stopped) {
        return;
    }
    uint32_t t_script = now_ms();
    const int w = ctx->api.w, h = ctx->api.h;

    /* Undo the previous frame, if the script froze a background: copy back
     * what it drew, and mark it so it goes out again. Without this a script
     * has to erase for itself, which only works over a flat colour. */
    lx_dirty_reset(&ctx->dirty);
    if (ctx->back) {
        if (ctx->prev.all) {
            memcpy(ctx->work, ctx->back, (size_t)w * h * 2);
            lx_dirty_all(&ctx->dirty);
        } else {
            for (int i = 0; i < ctx->prev.n; i++) {
                const lx_rect_t *r = &ctx->prev.r[i];
                lx_restore(ctx->work, ctx->back, w, r);
                lx_dirty_add(&ctx->dirty, r->x0, r->y0,
                             r->x1 - r->x0, r->y1 - r->y0);
            }
        }
    }

    for (int i = 0; i < ctx->n_ev && !ctx->stopped; i++) {
        deliver(ctx, &ctx->ev[i]);
    }
    ctx->n_ev = 0;

    if (!ctx->stopped && ctx->ref_tick != LUA_NOREF) {
        lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref_tick);
        lua_pushinteger(ctx->L, (lua_Integer)ctx->frame_dt);
        call_lua(ctx, 1, LUA_BUDGET_MS);
    }
    if (!ctx->stopped && ctx->ref_draw != LUA_NOREF) {
        lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref_draw);
        call_lua(ctx, 0, LUA_BUDGET_MS);
    }
    ctx->api.ms_script = (uint16_t)(now_ms() - t_script);

    /* What the script drew joins what was undone: together they are what
     * the screen has to be told about. Even after an error: the half frame
     * is what the script really drew, and the message goes on top. */
    if (ctx->drawn.all) {
        lx_dirty_all(&ctx->dirty);
    } else {
        for (int i = 0; i < ctx->drawn.n; i++) {
            const lx_rect_t *r = &ctx->drawn.r[i];
            lx_dirty_add(&ctx->dirty, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
        }
    }
    ctx->prev = ctx->drawn;         /* what to undo next frame */
    lx_dirty_reset(&ctx->drawn);
}

/* REQ_RESIZE: the canvas is new (the LVGL task made it, black, and dropped
 * the background, which was the old shape). The script is told the new size
 * and draws its world again. */
static void do_resize(lua_ctx_t *ctx)
{
    if (!ctx->L || ctx->stopped) {
        return;
    }
    lx_api_resize(ctx->L, &ctx->api);
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref_resize);
    lua_pushinteger(ctx->L, ctx->api.w);
    lua_pushinteger(ctx->L, ctx->api.h);
    call_lua(ctx, 2, LUA_LOAD_MS);
    lx_dirty_all(&ctx->dirty);
    lx_dirty_all(&ctx->drawn);
}

/* The thread. It sleeps a millisecond at a time until the LVGL task hands it
 * something, does it, and hands everything back. It only ever touches the
 * Lua state, the script's buffers and the context; never LVGL. */
static void lua_thread(void *arg)
{
    lua_ctx_t *ctx = (lua_ctx_t *)arg;
    for (;;) {
        if (ph_get(&ctx->phase) != PH_BUSY) {
            aos_hal_sleep_ms(1);
            continue;
        }
        switch (ctx->req) {
        case REQ_LOAD:   do_load(ctx);   break;
        case REQ_FRAME:  do_frame(ctx);  break;
        case REQ_RESIZE: do_resize(ctx); break;
        case REQ_QUIT:
            state_close(ctx);
            ph_set(&ctx->phase, PH_IDLE);
            ph_set(&ctx->exited, 1);    /* the last touch of ctx */
            return;
        default: break;
        }
        ph_set(&ctx->phase, PH_IDLE);
    }
}

/* ==========================================================================
 * The LVGL task's side
 * ========================================================================== */

static void post(lua_ctx_t *ctx, int req)
{
    ctx->req = req;
    ctx->fresh = true;
    ph_set(&ctx->phase, PH_BUSY);
}

/* Waits, blocking the LVGL task, for the thread to finish what it has. Only
 * for the moments that cannot go on otherwise -leaving, turning- and bounded
 * by the script's own budget. */
static bool wait_idle(lua_ctx_t *ctx, uint32_t ms)
{
    uint32_t t0 = now_ms();
    while (ph_get(&ctx->phase) == PH_BUSY) {
        if (now_ms() - t0 > ms) {
            return false;
        }
        aos_hal_sleep_ms(1);
    }
    return true;
}

static bool thread_start(lua_ctx_t *ctx)
{
    if (ctx->thread_on) {
        return true;
    }
    ph_set(&ctx->phase, PH_IDLE);
    ph_set(&ctx->exited, 0);
    ph_set(&ctx->cancel, 0);
    ctx->thread_on = aos_hal_thread_start("lua", lua_thread, ctx, LUA_STACK, LUA_PRIO);
    return ctx->thread_on;
}

/* Gets the thread back and ends it; the state is closed on its own stack.
 * false if it did not come back, and then nothing it can reach may be freed:
 * the hook makes that impossible in practice, and a leak beats a thread
 * writing into freed memory. */
static bool thread_stop(lua_ctx_t *ctx)
{
    if (!ctx->thread_on) {
        return true;
    }
    ph_set(&ctx->cancel, 1);
    bool ok = wait_idle(ctx, 3000);
    if (ok) {
        ctx->req = REQ_QUIT;
        ph_set(&ctx->phase, PH_BUSY);
        uint32_t t0 = now_ms();
        while (!ph_get(&ctx->exited) && now_ms() - t0 < 3000) {
            aos_hal_sleep_ms(1);
        }
        ok = ph_get(&ctx->exited) != 0;
    }
    if (!ok) {
        aos_hal_log("lua", "the script's thread did not come back; its memory is left alone");
        return false;
    }
    ctx->thread_on = false;
    ctx->fresh = false;
    ph_set(&ctx->cancel, 0);
    return true;
}

/* The rows the dirty list covers, for aos.stats(). */
static int rows_of(const lx_dirty_t *d)
{
    if (d->all) {
        return d->h;
    }
    int16_t y0[LX_MAX_DIRTY], y1[LX_MAX_DIRTY];
    int n = d->n;
    for (int i = 0; i < n; i++) {
        y0[i] = d->r[i].y0;
        y1[i] = d->r[i].y1;
    }
    /* Insertion sort by the top edge: n is at most LX_MAX_DIRTY, which is 18,
     * and this runs once a frame. */
    for (int i = 1; i < n; i++) {
        int16_t a = y0[i], b = y1[i];
        int j = i - 1;
        while (j >= 0 && y0[j] > a) {
            y0[j + 1] = y0[j];
            y1[j + 1] = y1[j];
            j--;
        }
        y0[j + 1] = a;
        y1[j + 1] = b;
    }
    int rows = 0, top = -1;
    for (int i = 0; i < n; i++) {
        int from = y0[i] > top ? y0[i] : top;
        if (y1[i] > from) {
            rows += y1[i] - from;
            top = y1[i];
        }
    }
    return rows;
}

static void run_script(lua_ctx_t *ctx, const char *name);
static void harvest(lua_ctx_t *ctx);

/* The running script again, from its file: when it changed on the card, or
 * when its error message is tapped. Only with the thread idle; otherwise it
 * stays asked for, and the next tick of the runtime tries again.
 *
 * Never from pump(). A reload may remake the canvas, and pump() runs inside
 * the canvas's own frame timer: ending the canvas there would delete the
 * timer that is calling. The runtime's tick and LVGL's events are outside
 * it. */
static void try_reload(lua_ctx_t *ctx)
{
    if (!ctx->want_reload || !ctx->running[0]) {
        return;
    }
    if (ctx->thread_on && ph_get(&ctx->phase) == PH_BUSY) {
        return;
    }
    harvest(ctx);               /* whatever the last request left */
    ctx->want_reload = false;
    char again[sizeof(ctx->running)];
    snprintf(again, sizeof(again), "%s", ctx->running);
    run_script(ctx, again);
}

static void reload_cb(lv_event_t *e)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_event_get_user_data(e);
    ctx->want_reload = true;
    try_reload(ctx);
}

static void show_error(lua_ctx_t *ctx, const char *what)
{
    if (!ctx->message) {
        int32_t w = lv_obj_get_width(ctx->root) - 2 * AOS_UI_PAD;
        if (w > 680) w = 680;
        ctx->message = lv_obj_create(ctx->root);
        lv_obj_remove_style_all(ctx->message);
        lv_obj_set_width(ctx->message, w);
        lv_obj_set_height(ctx->message, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(ctx->message, lv_color_hex(0x300000), 0);
        lv_obj_set_style_bg_opa(ctx->message, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ctx->message, 20, 0);
        lv_obj_set_style_pad_row(ctx->message, 12, 0);
        lv_obj_set_style_radius(ctx->message, AOS_UI_RADIUS / 2, 0);
        lv_obj_set_flex_flow(ctx->message, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(ctx->message, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ctx->message, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ctx->message, reload_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_align(ctx->message, LV_ALIGN_BOTTOM_MID, 0, -(LUA_EDGE_BOTTOM + AOS_UI_PAD));

        lv_obj_t *text = lv_label_create(ctx->message);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text, LV_PCT(100));
        lv_obj_set_style_text_color(text, lv_color_hex(0xFF9F9F), 0);
        lv_obj_set_style_text_font(text, aos_font_small, 0);

        /* The way on is the finger: the message itself reloads the
         * script (saving the file again does too). */
        lv_obj_t *hint = lv_label_create(ctx->message);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_text(hint, _("Tocá para cargarlo de nuevo"));
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(hint, aos_font_caption, 0);
        aos_make_decorative(text);
        aos_make_decorative(hint);
    }
    lv_label_set_text(lv_obj_get_child(ctx->message, 0), what);
    lv_obj_remove_flag(ctx->message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->message);
    aos_hal_beep(220, 120);
}

/* What the last request left: the pixels that changed go onto the canvas,
 * the beeps sound, the error shows. Only from the frame timer (or before
 * turning), with the thread idle. */
static void harvest(lua_ctx_t *ctx)
{
    if (!ctx->fresh) {
        return;
    }
    ctx->fresh = false;

    const aos_retro_t *r = ctx->r;
    if (r && ctx->work) {
        const int w = r->w;
        if (ctx->dirty.all) {
            memcpy(r->px, ctx->work, (size_t)w * r->h * 2);
            aos_retro_present();
        } else {
            for (int i = 0; i < ctx->dirty.n; i++) {
                const lx_rect_t *d = &ctx->dirty.r[i];
                lx_restore(r->px, ctx->work, w, d);
                aos_retro_present_rect(d->x0, d->y0, d->x1 - d->x0, d->y1 - d->y0);
            }
        }
        ctx->api.rows = (uint16_t)rows_of(&ctx->dirty);
        ctx->api.px = (uint32_t)lx_dirty_area(&ctx->dirty);
        lx_dirty_reset(&ctx->dirty);
    }

    for (int i = 0; i < ctx->api.n_beeps; i++) {
        aos_hal_beep(ctx->api.beeps[i].hz, ctx->api.beeps[i].ms);
    }
    ctx->api.n_beeps = 0;

    ctx->want_touch = ctx->has_touch;
    ctx->want_gesture = ctx->has_gesture;
    if (ctx->err_new) {
        ctx->err_new = false;
        show_error(ctx, ctx->err);
    }
}

/* The finger, in the script's coordinates: it never learns how many screen
 * pixels a canvas pixel is. */
static int16_t to_canvas(int v, int origin, int k)
{
    int d = v - origin;
    return (int16_t)(d >= 0 ? d / k : -((-d + k - 1) / k));
}

static void queue(lua_ctx_t *ctx, const lua_ev_t *e)
{
    /* A move replaces the move before it: the script wants where the finger
     * is, not every sample on the way, and a slow frame would otherwise fill
     * the queue with them. */
    if (e->kind == EV_MOVE && ctx->n_pend > 0 && ctx->pend[ctx->n_pend - 1].kind == EV_MOVE) {
        ctx->pend[ctx->n_pend - 1] = *e;
        return;
    }
    if (ctx->n_pend < LUA_MAX_EVENTS) {
        ctx->pend[ctx->n_pend++] = *e;
    }
}

static void touch_cb(lv_event_t *e)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!ctx->r || !ctx->surface) {
        return;
    }

    lv_point_t p = { 0, 0 };
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    lv_area_t a;
    lv_obj_get_coords(ctx->surface, &a);
    int k = ctx->r->scale;

    ctx->t_x    = to_canvas(p.x, a.x1, k);
    ctx->t_y    = to_canvas(p.y, a.y1, k);
    ctx->t_down = (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING);

    /* LVGL sends PRESSING on every frame the finger stays put; only a finger
     * that moved is a "move" for the script. */
    static int16_t last_x, last_y;
    lua_ev_t ev = { 0 };
    ev.x = ctx->t_x;
    ev.y = ctx->t_y;
    if (code == LV_EVENT_PRESSED) {
        ev.kind = EV_DOWN;
    } else if (code == LV_EVENT_PRESSING) {
        if (ev.x == last_x && ev.y == last_y) return;
        ev.kind = EV_MOVE;
    } else {
        ev.kind = EV_UP;
    }
    last_x = ev.x;
    last_y = ev.y;
    if (ctx->want_touch && !ctx->stopped) {
        queue(ctx, &ev);
    }
}

/* The recogniser's events (aos_gesture.h), for a script that defines
 * gesture(). Positions and distances in the script's pixels, like touch();
 * a pinch's scale is a plain factor, to multiply into the script's zoom. */
static void gesture_cb(const aos_gesture_event_t *g, void *user)
{
    lua_ctx_t *ctx = (lua_ctx_t *)user;
    if (!ctx->want_gesture || ctx->stopped || !ctx->r || !ctx->surface) {
        return;
    }
    lua_ev_t ev = { 0 };
    ev.kind = EV_GESTURE;
    switch (g->type) {
    case AOS_GESTURE_TAP:         ev.name = "tap";        break;
    case AOS_GESTURE_DOUBLE_TAP:  ev.name = "double";     break;
    case AOS_GESTURE_LONG_PRESS:  ev.name = "long";       break;
    case AOS_GESTURE_DRAG:        ev.name = "drag";       ev.nargs = 2; break;
    case AOS_GESTURE_DRAG_END:    ev.name = "release";    ev.nargs = 2; break;
    case AOS_GESTURE_PINCH_BEGIN: ev.name = "pinchstart"; break;
    case AOS_GESTURE_PINCH:       ev.name = "pinch";      ev.nargs = 3; break;
    case AOS_GESTURE_PINCH_END:   ev.name = "pinchend";   break;
    default: return;
    }
    lv_area_t a;
    lv_obj_get_coords(ctx->surface, &a);
    float k = (float)ctx->r->scale;
    ev.x = to_canvas((int)g->x, a.x1, ctx->r->scale);
    ev.y = to_canvas((int)g->y, a.y1, ctx->r->scale);
    if (g->type == AOS_GESTURE_DRAG) {
        ev.a = g->dx / k;
        ev.b = g->dy / k;
    } else if (g->type == AOS_GESTURE_DRAG_END) {
        ev.a = g->vx / k;
        ev.b = g->vy / k;
    } else if (g->type == AOS_GESTURE_PINCH) {
        ev.a = g->scale;
        ev.b = g->dx / k;
        ev.c = g->dy / k;
    }
    /* Several drags or pinches can come in one frame (the chip sends ~73
     * samples a second): they add up into one, which is what the script
     * would do with them anyway. */
    if (ctx->n_pend > 0) {
        lua_ev_t *last = &ctx->pend[ctx->n_pend - 1];
        if (last->kind == EV_GESTURE && last->name == ev.name) {
            if (g->type == AOS_GESTURE_DRAG) {
                last->a += ev.a;
                last->b += ev.b;
                last->x = ev.x;
                last->y = ev.y;
                return;
            }
            if (g->type == AOS_GESTURE_PINCH) {
                last->a *= ev.a;
                last->b += ev.b;
                last->c += ev.c;
                last->x = ev.x;
                last->y = ev.y;
                return;
            }
        }
    }
    queue(ctx, &ev);
}

/* The frame timer's callback (aos_retro_run, LVGL task). If the thread is
 * still on the last frame, nothing: the script is slower than the timer, and
 * it gets the frames it can make. Otherwise, what it made goes to the screen
 * and the next one is handed over with a snapshot of the finger.
 *
 * Between two frames the thread sleeps at least a millisecond (lua_thread),
 * which is what keeps a script slower than the timer from starving the
 * lower-priority tasks on its core: the runtime's tick among them. */
static void pump(void *user)
{
    lua_ctx_t *ctx = (lua_ctx_t *)user;
    if (!ctx->thread_on || ph_get(&ctx->phase) == PH_BUSY) {
        return;
    }
    harvest(ctx);

    if (ctx->want_reload || ctx->stopped || !ctx->r) {
        return;                 /* a reload is try_reload()'s, not ours */
    }

    uint32_t now = now_ms();
    uint32_t dt = now - ctx->last_post;
    ctx->last_post = now;
    ctx->frame_dt = dt;
    ctx->api.ms_frame = (uint16_t)(dt > 0xFFFF ? 0xFFFF : dt);
    aos_retro_stats_t st;
    aos_retro_stats(&st);
    ctx->api.ms_screen = (uint16_t)(st.refresh_us_avg / 1000);

    ctx->api.touch_x = ctx->t_x;
    ctx->api.touch_y = ctx->t_y;
    ctx->api.touch_down = ctx->t_down;
    ctx->api.pad_held = (uint16_t)ctx->pad.held;
    ctx->api.pad_pressed = ctx->pad_pend;
    ctx->api.pad_x = ctx->pad.x;
    ctx->api.pad_y = ctx->pad.y;
    ctx->api.pad_on = ctx->pad.connected;
    ctx->pad_pend = 0;

    /* aos.fingers(): read here, in the LVGL task where aos_touch_points()
     * has to be called, in canvas pixels. */
    ctx->api.fingers = 0;
    if (ctx->surface) {
        aos_touch_point_t pts[2];
        aos_touch_points(pts);
        lv_area_t a;
        lv_obj_get_coords(ctx->surface, &a);
        for (int i = 0; i < 2; i++) {
            if (!pts[i].down) continue;
            int n = ctx->api.fingers++;
            ctx->api.finger_id[n] = pts[i].id;
            ctx->api.finger_x[n]  = to_canvas((int)pts[i].x, a.x1, ctx->r->scale);
            ctx->api.finger_y[n]  = to_canvas((int)pts[i].y, a.y1, ctx->r->scale);
        }
    }

    memcpy(ctx->ev, ctx->pend, sizeof(lua_ev_t) * (size_t)ctx->n_pend);
    ctx->n_ev = ctx->n_pend;
    ctx->n_pend = 0;
    post(ctx, REQ_FRAME);
}

/* ==========================================================================
 * The canvas
 * ========================================================================== */

/* The canvas for the root's shape and the script's head, the buffers that go
 * with it, and the surface over it. With the thread idle. 'keep' is for a
 * canvas of a fixed size remade after turning: the script's pixels carry
 * over as they are, and the script never learns. */
static bool canvas_setup(lua_ctx_t *ctx, bool keep)
{
    lv_obj_t *root = ctx->root;
    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);

    int w = ctx->head.w, h = ctx->head.h;
    if (w <= 0 || h <= 0) {
        /* fill: 240x426 upright, 426x240 lying down, each x3 */
        w = W / LUA_FILL_DIV;
        h = H / LUA_FILL_DIV;
    }

    if (ctx->surface) {
        lv_obj_delete(ctx->surface);
        ctx->surface = NULL;
    }
    ctx->r = aos_retro_begin(root, w, h, 0, 0);
    if (!ctx->r) {
        return false;
    }

    bool same = keep && ctx->work && ctx->api.w == w && ctx->api.h == h;
    if (!same) {
        free(ctx->work);
        free(ctx->back);
        ctx->back = NULL;
        /* malloc and not lv_malloc: big means PSRAM, which is where a canvas
         * belongs (docs/MEMORY.md) */
        ctx->work = (uint16_t *)malloc((size_t)w * h * 2);
        if (!ctx->work) {
            aos_retro_end();
            ctx->r = NULL;
            return false;
        }
        memset(ctx->work, 0, (size_t)w * h * 2);
        lx_buf_init(&ctx->buf, ctx->work, w, h);
        lx_dirty_init(&ctx->drawn, w, h);
        lx_dirty_init(&ctx->dirty, w, h);
        lx_dirty_init(&ctx->prev, w, h);
        ctx->api.w = (int16_t)w;
        ctx->api.h = (int16_t)h;
    } else {
        memcpy(ctx->r->px, ctx->work, (size_t)w * h * 2);
        aos_retro_present();
    }
    ctx->api.buf    = &ctx->buf;
    ctx->api.dirty  = &ctx->drawn;
    ctx->api.freeze = freeze_background;
    ctx->api.app    = ctx;

    /* The finger's object, exactly over the canvas. The canvas's own view
     * takes no clicks; this does, and so does the gesture recogniser on it,
     * which only listens to touches that start on its object. */
    ctx->surface = lv_obj_create(root);
    lv_obj_remove_style_all(ctx->surface);
    lv_obj_set_pos(ctx->surface, ctx->r->x, ctx->r->y);
    lv_obj_set_size(ctx->surface, ctx->r->w * ctx->r->scale, ctx->r->h * ctx->r->scale);
    lv_obj_remove_flag(ctx->surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctx->surface, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ctx->surface, touch_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(ctx->surface, touch_cb, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(ctx->surface, touch_cb, LV_EVENT_RELEASED, ctx);
    aos_gesture_attach(ctx->surface, 0, gesture_cb, ctx);
    if (ctx->message) {
        lv_obj_move_foreground(ctx->message);
    }

    ctx->last_post = now_ms();
    aos_retro_run(LUA_FPS, NULL, pump, ctx);
    aos_hal_log("lua", "%s: canvas %dx%d x%d", ctx->running, w, h, ctx->r->scale);
    return true;
}

/* A message on a black screen, for what fails before there is a script to
 * blame (no memory for the canvas or the thread). */
static void fail_here(lua_ctx_t *ctx, const char *what)
{
    snprintf(ctx->err, sizeof(ctx->err), "%s", what);
    show_error(ctx, ctx->err);
}

static void run_script(lua_ctx_t *ctx, const char *name)
{
    /* Copied first: 'name' may be ctx->running itself when this is a
     * reload. */
    char wanted[sizeof(ctx->running)];
    snprintf(wanted, sizeof(wanted), "%s", name);
    snprintf(ctx->running, sizeof(ctx->running), "%s", wanted);

    const char *sd = aos_hal_path_sd_root();
    char path[160];
    snprintf(path, sizeof(path), "%s/lua/%s", sd ? sd : "/sdcard", wanted);
    struct stat st;
    if (stat(path, &st) == 0) {
        ctx->watch_mtime = st.st_mtime;
        ctx->watch_size  = (long)st.st_size;
    }
    ctx->watch_at = lv_tick_get();

    lua_head_t old = ctx->head;
    head_read(wanted, &ctx->head);

    if (ctx->list) {
        lv_obj_clean(ctx->root);
        ctx->list = NULL;
        ctx->message = NULL;
        ctx->surface = NULL;
    }
    /* the list's buttons are gone, and the A that picked the script is
     * not the script's first press */
    aos_pad_menu_clear(&ctx->pad_menu);
    aos_pad_reset(&ctx->pad, lv_tick_get());
    ctx->pad_pend = 0;
    if (ctx->message) {
        lv_obj_add_flag(ctx->message, LV_OBJ_FLAG_HIDDEN);
    }
    ctx->n_pend = 0;
    ctx->stopped = false;
    ctx->err_new = false;
    ctx->want_touch = ctx->want_gesture = false;

    /* A reload keeps the canvas when the head still asks for the same one;
     * the pixels are wiped either way, because a new script starts black. */
    bool same = ctx->r && old.w == ctx->head.w && old.h == ctx->head.h;
    if (!same) {
        if (!canvas_setup(ctx, false)) {
            lv_obj_set_style_bg_color(ctx->root, lv_color_hex(0x000000), 0);
            fail_here(ctx, "no memory for the canvas");
            return;
        }
    } else {
        free(ctx->back);
        ctx->back = NULL;
        memset(ctx->work, 0, (size_t)ctx->api.w * ctx->api.h * 2);
        lx_dirty_reset(&ctx->drawn);
        lx_dirty_reset(&ctx->prev);
        lx_dirty_all(&ctx->dirty);
    }

    if (!thread_start(ctx)) {
        fail_here(ctx, "no memory for the interpreter's thread");
        return;
    }
    post(ctx, REQ_LOAD);
}

/* ==========================================================================
 * The list
 * ========================================================================== */

static void pick_async(void *arg)
{
    lua_ctx_t *ctx = (lua_ctx_t *)arg;
    int idx = ctx->pick;
    if (idx >= 0 && idx < ctx->count && ctx->list) {
        run_script(ctx, ctx->names[idx]);
    }
}

/* Deferred, because running a script deletes the list, and with it the
 * button whose event this is. */
static void pick_cb(lv_event_t *e)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t  *btn = (lv_obj_t *)lv_event_get_target(e);
    ctx->pick = (int)(intptr_t)lv_obj_get_user_data(btn);
    lv_async_call(pick_async, ctx);
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan(lua_ctx_t *ctx)
{
    ctx->count = 0;
    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        return;
    }
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/lua", sd);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && ctx->count < LUA_MAX_SCRIPTS) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".lua") != 0 || e->d_name[0] == '.') {
            continue;
        }
        /* A name that does not fit is skipped rather than truncated: a
         * truncated one would sit in the list and then fail to open, which
         * is a worse bug than not being listed. */
        size_t n = strlen(e->d_name);
        if (n >= sizeof(ctx->names[0])) {
            continue;
        }
        memcpy(ctx->names[ctx->count], e->d_name, n + 1);
        ctx->count++;
    }
    closedir(d);
    qsort(ctx->names, (size_t)ctx->count, sizeof(ctx->names[0]), by_name);
}

static void build_list(lua_ctx_t *ctx)
{
    if (ctx->r) {
        aos_retro_end();
        ctx->r = NULL;
    }
    lv_obj_clean(ctx->root);
    ctx->surface = NULL;
    ctx->message = NULL;
    lv_obj_set_style_bg_color(ctx->root, lv_color_hex(0x05050C), 0);
    lv_obj_set_style_bg_opa(ctx->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ctx->root, LV_OBJ_FLAG_SCROLLABLE);

    scan(ctx);

    /* The whole root, scrolling; the app is full screen, so the top keeps
     * clear of the strip that pulls the panels down, and the bottom of the
     * one that goes home. */
    ctx->list = lv_obj_create(ctx->root);
    lv_obj_remove_style_all(ctx->list);
    lv_obj_set_size(ctx->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(ctx->list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(ctx->list, 72, 0);
    lv_obj_set_style_pad_bottom(ctx->list, LUA_EDGE_BOTTOM + AOS_UI_PAD, 0);
    lv_obj_set_style_pad_row(ctx->list, 14, 0);
    lv_obj_set_scroll_dir(ctx->list, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(ctx->list);
    lv_label_set_text(title, "Lua");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, aos_font_title, 0);
    lv_obj_set_style_pad_bottom(title, 10, 0);

    aos_pad_menu_clear(&ctx->pad_menu);
    if (ctx->count == 0) {
        lv_obj_t *empty = lv_label_create(ctx->list);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text(empty, _("No hay guiones en /lua de la tarjeta"));
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(empty, aos_font_body, 0);
        return;
    }

    lv_obj_t *btns[AOS_PAD_MENU_MAX];
    for (int i = 0; i < ctx->count; i++) {
        lv_obj_t *btn = lv_button_create(ctx->list);
        lv_obj_set_size(btn, LV_PCT(100), AOS_UI_ROW_H);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C1C2E), 0);
        lv_obj_set_style_radius(btn, AOS_UI_RADIUS / 2, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, AOS_UI_PAD, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, pick_cb, LV_EVENT_CLICKED, ctx);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, ctx->names[i]);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8E8F0), 0);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        /* the pad's menu holds the first AOS_PAD_MENU_MAX; it scrolls the
         * list to each one it moves to */
        if (i < AOS_PAD_MENU_MAX) btns[i] = btn;
    }
    aos_pad_menu_set(&ctx->pad_menu, btns,
                     ctx->count < AOS_PAD_MENU_MAX ? ctx->count : AOS_PAD_MENU_MAX, 0);
    aos_pad_reset(&ctx->pad, lv_tick_get());
}

/* ==========================================================================
 * The gamepad
 * ========================================================================== */

/* Every 20 ms, in the LVGL task, whatever is on screen: one read of the pad.
 * Over the list it drives the buttons; over a script it gathers the presses
 * for the next frame - pump() only runs when the thread is free, and a press
 * that came and went during a slow frame would be lost there. On an error
 * message, A is the tap that reloads. */
static void pad_cb(lv_timer_t *t)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_timer_get_user_data(t);

    aos_pad_update(&ctx->pad, lv_tick_get());
    if (ctx->list && !ctx->r) {
        (void)aos_pad_menu_step(&ctx->pad_menu, &ctx->pad);
        return;
    }
    ctx->pad_pend = (uint16_t)(ctx->pad_pend | ctx->pad.pressed);
    if (ctx->message && !lv_obj_has_flag(ctx->message, LV_OBJ_FLAG_HIDDEN) &&
        aos_pad_pressed(&ctx->pad, AOS_PAD_A)) {
        ctx->want_reload = true;
        try_reload(ctx);
    }
}

static void pad_start(lua_ctx_t *ctx)
{
    aos_pad_reset(&ctx->pad, lv_tick_get());
    ctx->pad_timer = lv_timer_create(pad_cb, 20, ctx);
}

/* ==========================================================================
 * The app
 * ========================================================================== */

static void *lua_create(aos_app_t *self, lv_obj_t *root)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_malloc_zeroed(sizeof(lua_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->root = root;
    ctx->self = self;
    ctx->ref_tick = ctx->ref_draw = ctx->ref_touch = LUA_NOREF;
    ctx->ref_gesture = ctx->ref_resize = LUA_NOREF;
    pad_start(ctx);

    /* LUA_RUN=cubo.lua opens straight into that script. On the board getenv
     * always returns NULL -there is no environment- so this costs nothing and
     * travels in the same binary. */
    const char *run = getenv("LUA_RUN");
    if (run && *run) {
        run_script(ctx, run);
        return ctx;
    }

    build_list(ctx);
    return ctx;
}

static void lua_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    lua_ctx_t *ctx = (lua_ctx_t *)inst;
    if (!ctx) {
        return;
    }
    /* The thread first: it reaches into the state and the buffers, which are
     * about to go. The LVGL objects under root are the runtime's business;
     * the canvas is ended here so its timers stop calling pump(). */
    lv_async_call_cancel(pick_async, ctx);  /* a tap on the list in flight */
    if (ctx->pad_timer) {
        lv_timer_delete(ctx->pad_timer);
        ctx->pad_timer = NULL;
    }
    bool back = thread_stop(ctx);
    aos_retro_end();
    if (!back) {
        return;
    }
    free(ctx->work);
    free(ctx->back);
    lv_free(ctx);
}

/* Called about five times a second by the runtime. It watches the file the
 * running script came from and reloads when it changes on the card.
 *
 * Checked once a second and not on every call: a stat() goes through FatFs to
 * the card. FAT keeps the time to the nearest two seconds, which is why the
 * size counts too -two edits within the same second usually change the
 * length- and why a save that changes neither is missed. That is the price of
 * doing this with no help from the firmware, and it is cheap: saving again
 * picks it up. */
static void lua_watch(aos_app_t *self, void *inst)
{
    (void)self;
    lua_ctx_t *ctx = (lua_ctx_t *)inst;
    if (!ctx || !ctx->running[0]) {
        return;
    }
    try_reload(ctx);            /* one asked for while the thread was busy */
    if (!ctx->r || lv_tick_elaps(ctx->watch_at) < 1000) {
        return;
    }
    ctx->watch_at = lv_tick_get();

    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/lua/%s", sd, ctx->running);
    struct stat st;
    if (stat(path, &st) != 0) {
        return;                 /* deleted while running: leave it alone */
    }
    if (st.st_mtime == ctx->watch_mtime && (long)st.st_size == ctx->watch_size) {
        return;
    }
    ctx->watch_mtime = st.st_mtime;
    ctx->watch_size  = (long)st.st_size;
    ctx->want_reload = true;
    try_reload(ctx);
}

/* Back (the left edge) goes from a script to the list, and only then out of
 * the app. */
static bool lua_back(aos_app_t *self, void *inst)
{
    (void)self;
    lua_ctx_t *ctx = (lua_ctx_t *)inst;
    /* A script opened from the launcher has no list behind it: back leaves
     * the app, which is what every other app does. */
    if (!ctx || ctx->standalone) {
        return false;
    }
    if (ctx->r || ctx->thread_on) {
        if (!thread_stop(ctx)) {
            return false;       /* leave the app instead: destroy() copes */
        }
        ctx->running[0] = '\0';
        memset(&ctx->head, 0, sizeof(ctx->head));
        build_list(ctx);
        return true;
    }
    return false;
}

/* The screen turned, and the root already has its new size.
 *
 *   - the list is made again;
 *   - a script with a canvas of its own size keeps it, pixels and all: the
 *     canvas is only placed again for the new shape;
 *   - a script on the default canvas gets the new shape (426x240 lying
 *     down), black. If it defines resize(w, h) it is told and carries on
 *     with its state; if not, it starts again, which for a script written
 *     for one size is the only honest thing. */
static bool lua_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    lua_ctx_t *ctx = (lua_ctx_t *)inst;
    if (!ctx) {
        return false;
    }
    if (!ctx->running[0]) {
        build_list(ctx);
        return true;
    }
    if (!wait_idle(ctx, 1000)) {
        return false;           /* the runtime makes the app again */
    }
    harvest(ctx);

    if (ctx->head.w > 0) {
        return canvas_setup(ctx, true);
    }
    bool resume = ctx->has_resize && !ctx->stopped && ctx->thread_on;
    if (!canvas_setup(ctx, false)) {
        return false;
    }
    if (resume) {
        post(ctx, REQ_RESIZE);
    } else if (ctx->thread_on) {
        post(ctx, REQ_LOAD);
    }
    return true;
}

/* ==========================================================================
 * One app per script
 *
 * The module tells the loader how many apps it brings and describes each one,
 * so a .lua on the card is an entry in the launcher with its name, its colour
 * and its icon, beside the apps written in C. The interpreter is paid for
 * once and every script after that is data.
 *
 * The index is NOT the identity. The loader writes down which app of the
 * module a slot is and asks for that index again when it reopens it, but by
 * then a script may have been added or deleted and the indices will have
 * moved. What an app IS comes from its id -"lua.cubo" is cubo.lua- which the
 * runtime keeps and which does not move. The scan is sorted for the same
 * reason the id exists: so that the same card gives the same answer twice.
 * ========================================================================== */

/* The scripts, sorted, as of the last time anyone asked. Filled by
 * app_scan(), which is called from aos_app_count() -the loader's first
 * question- and again from each describe, because on the board the module is
 * opened, asked, and closed. */
static char s_apps_names[LUA_MAX_APPS][48];
static int  s_apps_count;

/* Which scripts have an icon file next to them, seen in the same readdir.
 * Measured on the watch: with 200 scripts in the folder, every fopen walks
 * the FAT directory and costs ~25 ms, and asking for an .aic that is not
 * there cost as much as reading the script. Past AIC_SEEN_MAX the set is
 * not trusted and app_icon() goes back to asking the card. */
#define AIC_SEEN_MAX 64
static char s_aic_names[AIC_SEEN_MAX][48];
static int  s_aic_count;
static bool s_aic_overflow;

static void app_scan(void)
{
    s_apps_count = 0;
    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        return;
    }
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/lua", sd);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    s_aic_count = 0;
    s_aic_overflow = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcasecmp(dot, ".aic") == 0) {
            size_t base = (size_t)(dot - e->d_name);
            if (s_aic_count < AIC_SEEN_MAX && base < sizeof(s_aic_names[0])) {
                memcpy(s_aic_names[s_aic_count], e->d_name, base);
                s_aic_names[s_aic_count][base] = '\0';
                s_aic_count++;
            } else {
                s_aic_overflow = true;
            }
            continue;
        }
        if (!dot || strcasecmp(dot, ".lua") != 0 || e->d_name[0] == '.' ||
            s_apps_count >= LUA_MAX_APPS) {
            continue;
        }
        /* Short enough that "lua." plus the name still fits in the loader's
         * 40-byte id. If it did not, the id would be TRUNCATED there and
         * create() would look for a file that does not exist -an app in the
         * launcher that opens onto an error. A long name is still in the
         * list inside this app, where nothing depends on its length. */
        size_t n = strlen(e->d_name);
        if (n >= sizeof(s_apps_names[0]) || n + 4 >= 40) {
            continue;
        }
        memcpy(s_apps_names[s_apps_count], e->d_name, n + 1);
        s_apps_count++;
    }
    closedir(d);
    qsort(s_apps_names, (size_t)s_apps_count, sizeof(s_apps_names[0]), by_name);

    /* Said out loud because the launcher is built once, at boot: a script
     * copied to the card afterwards is in the list inside this app straight
     * away and in the launcher only after a restart, and without this line
     * there is no way to tell that from a script the scan refused. */
    aos_hal_log("lua", "%s: %d script%s for the launcher",
                dir, s_apps_count, s_apps_count == 1 ? "" : "s");
}

/* A colour per script, from its name. Not decoration: fifteen identical tiles
 * in the launcher are fifteen tiles you have to read one by one. */
static uint32_t app_hue(const char *name, bool second)
{
    static const uint32_t PAIRS[][2] = {
        { 0x0A84FF, 0x0050A0 }, { 0x30D158, 0x1A7F36 }, { 0xFF9F0A, 0xB36A00 },
        { 0xFF375F, 0xA61E3A }, { 0xBF5AF2, 0x7A2FA0 }, { 0x64D2FF, 0x2E8FB0 },
        { 0xFFD60A, 0xB39400 }, { 0x5E5CE6, 0x3A38A0 },
    };
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    return PAIRS[h % (sizeof(PAIRS) / sizeof(PAIRS[0]))][second ? 1 : 0];
}

/* An .aic beside the script gives it an icon, with no firmware and no
 * reflashing (tools/aic.py). /sdcard/icons/<id>.aic still works too and
 * wins, because that is the firmware's own override. */
static bool aic_listed(const char *file)
{
    if (s_aic_overflow) {
        return true;                    /* unknown: ask the card */
    }
    const char *dot = strrchr(file, '.');
    size_t base = dot ? (size_t)(dot - file) : strlen(file);
    for (int i = 0; i < s_aic_count; i++) {
        if (strlen(s_aic_names[i]) == base && strncmp(s_aic_names[i], file, base) == 0) {
            return true;
        }
    }
    return false;
}

static void app_icon(aos_app_t *app, const char *file)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd || !aic_listed(file)) {
        return;
    }
    char path[176];
    snprintf(path, sizeof(path), "%s/lua/%s", sd, file);
    char *dot = strrchr(path, '.');
    if (!dot) {
        return;
    }
    snprintf(dot, sizeof(path) - (size_t)(dot - path), ".aic");

    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len > 0 && len <= 2048) {
        uint8_t *blob = (uint8_t *)malloc((size_t)len);
        if (blob) {
            if (fread(blob, 1, (size_t)len, f) == (size_t)len) {
                aos_icon_set_ops(app, blob, (size_t)len);   /* it copies it */
            }
            free(blob);
        }
    }
    fclose(f);
}

/* The script this app is, from its id: "lua.cubo" -> "cubo.lua". */
static void app_file_of(const char *id, char *out, size_t out_len)
{
    const char *base = id && strncmp(id, "lua.", 4) == 0 ? id + 4 : id;
    snprintf(out, out_len, "%s.lua", base ? base : "");
}

static void *script_create(aos_app_t *self, lv_obj_t *root)
{
    lua_ctx_t *ctx = (lua_ctx_t *)lv_malloc_zeroed(sizeof(lua_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->root = root;
    ctx->self = self;
    ctx->standalone = true;
    ctx->ref_tick = ctx->ref_draw = ctx->ref_touch = LUA_NOREF;
    ctx->ref_gesture = ctx->ref_resize = LUA_NOREF;
    pad_start(ctx);

    char file[sizeof(ctx->running)];
    app_file_of(self->desc.id, file, sizeof(file));
    run_script(ctx, file);
    return ctx;
}

/* ========================================================================== */

static bool describe_list(aos_app_t *app)
{
    app->desc.id       = "aos.lua";
    app->desc.name     = "Lua";
    app->desc.icon     = "Lua";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2C2D72;
    app->desc.color_b  = 0x000080;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;
    app->desc.order    = 900;

    app->create  = lua_create;
    app->destroy = lua_destroy;
    app->back    = lua_back;
    app->tick    = lua_watch;
    app->resize  = lua_resize;
    return true;
}

/* The strings of a descriptor have to outlive this call, and one set of
 * statics is NOT enough.
 *
 * aos_ui_register_app() does `s_apps[n] = *app`, a struct copy: it keeps the
 * POINTERS. On the board that is harmless, because the loader copies the
 * strings into a slot of its own first; but the simulator registers what the
 * module hands it, and with one shared buffer every script app ended up
 * pointing at the last name written. The symptom was a single line -"duplicate
 * app: lua.hola"- and the scripts missing from the launcher.
 *
 * One buffer per app, then.
 *
 * The id is 52: four for "lua." plus the longest file name app_scan() lets
 * through. The loader copies it into a 40-byte field of its own, and the scan
 * refuses anything that would not fit there -truncating the id is how you get
 * an app in the launcher that opens onto a file that does not exist. */
static char s_ids[LUA_MAX_APPS][52];
static char s_names[LUA_MAX_APPS][48];

static uint32_t lua_count(void)
{
    app_scan();
    return 1u + (uint32_t)s_apps_count;
}

static bool lua_describe(aos_app_t *app, uint32_t index)
{
    if (index == 0) {
        return describe_list(app);
    }
    if (s_apps_count == 0) {
        app_scan();             /* reopened on the board: scan again */
    }
    uint32_t i = index - 1;
    if (i >= (uint32_t)s_apps_count) {
        return false;
    }
    const char *file = s_apps_names[i];

    char *id   = s_ids[i];
    char *name = s_names[i];

    snprintf(id, sizeof(s_ids[0]), "lua.%s", file);
    char *dot = strrchr(id, '.');
    if (dot && strcasecmp(dot, ".lua") == 0) {
        *dot = '\0';
    }
    lua_head_t head;
    head_read(file, &head);
    snprintf(name, sizeof(s_names[0]), "%s", head.name);

    app->desc.id       = id;
    app->desc.name     = name;
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = app_hue(file, false);
    app->desc.color_b  = app_hue(file, true);
    /* The orientation a script asked for is the launcher entry's: the
     * runtime turns the screen when an app opens, and a script run from the
     * list inside this app opens in whatever the list was in. */
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | head.orient;
    app->desc.order    = 901 + (int32_t)i;

    app->create  = script_create;
    app->destroy = lua_destroy;
    app->back    = lua_back;
    app->tick    = lua_watch;
    app->resize  = lua_resize;

    app_icon(app, file);
    return true;
}

AOS_APP_ENTRY_MANY(lua_count, lua_describe);
