/*
 * AmoledOS - The launcher's order and folders (docs/MENU.md).
 *
 * One text file, menu.txt, written by the portal's /menu page and only read
 * here. A line per entry, in the order the launcher shows them:
 *
 *     # comments and blank lines are ignored
 *     app aos.settings
 *     folder f1 7B2FF7 F107A3 v gamepad-variant w Juegos
 *       app demo.turbo
 *       app demo.golf
 *     end
 *
 * folder <id> <color A> <color B> <fill> <glyph> <glyph colour> <name...>
 *   fill          s solid, v vertical, d diagonal, r radial
 *   glyph         a name from the folder glyph catalogue (aos_folder_icon.h)
 *   glyph colour  w white, b black
 *   name          the rest of the line, as the user wrote it
 *
 * What the file does not mention still shows: an installed app that is in no
 * line goes at the end of the top level, in its usual order. That is where a
 * newly installed app lands. And what the file mentions but is not installed
 * is skipped, but it stays in the file: put the app back on the card and it
 * comes back to its place.
 *
 * One level of folders, and an app in one place: a second line for the same
 * id is ignored.
 *
 *     hide demo.hello
 *
 * keeps an installed app out of the launcher altogether, wherever else the
 * file names it. It is still installed: the portal opens it, and deleting
 * the line brings it back.
 */
#pragma once

#include "aos_app.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_MENU_FOLDERS_MAX    32
#define AOS_MENU_FOLDER_ID_MAX  16
#define AOS_MENU_NAME_MAX       40      /* bytes, UTF-8, terminator included */
#define AOS_MENU_GLYPH_MAX      32
#define AOS_MENU_FILE_MAX       (24 * 1024)

typedef enum {
    AOS_MENU_FILL_SOLID = 0,
    AOS_MENU_FILL_VER,
    AOS_MENU_FILL_DIAG,
    AOS_MENU_FILL_RADIAL,
} aos_menu_fill_t;

typedef struct {
    char     id[AOS_MENU_FOLDER_ID_MAX];
    char     name[AOS_MENU_NAME_MAX];
    char     glyph[AOS_MENU_GLYPH_MAX];
    uint32_t color_a;
    uint32_t color_b;
    uint8_t  fill;                      /* aos_menu_fill_t */
    bool     glyph_dark;
} aos_menu_folder_t;

/* P4OS additions to the file (docs in UI.md):
 *
 *     dock                      the apps of the dock, in order
 *       app <id>
 *     end
 *     page                      forces the next item onto a new page
 *     widget <type> <w>x<h> [arg]   a home widget, w x h grid cells
 */
#define AOS_MENU_WIDGET_TYPE_MAX 16
#define AOS_MENU_WIDGET_ARG_MAX  64
#define AOS_MENU_DOCK_MAX        6

typedef enum {
    AOS_MENU_ITEM_APP = 0,
    AOS_MENU_ITEM_FOLDER,
    AOS_MENU_ITEM_PAGE,     /* page break */
    AOS_MENU_ITEM_WIDGET,
} aos_menu_kind_t;

/* One slot of a resolved level: an app, a folder (app NULL), a page break or
 * a widget. */
typedef struct {
    const aos_app_t *app;
    int              folder;            /* index for aos_menu_folder(), -1 for an app */
    uint8_t          kind;              /* aos_menu_kind_t */
    uint8_t          w, h;              /* widget size in cells */
    char             widget[AOS_MENU_WIDGET_TYPE_MAX];
    char             arg[AOS_MENU_WIDGET_ARG_MAX];
} aos_menu_item_t;

/* The dock's installed apps, in the file's order (at most AOS_MENU_DOCK_MAX).
 * An app in the dock does not show on the pages. */
int aos_menu_dock(const aos_app_t **out, int max);

/* Whether the file had a dock section at all (with no file, the runtime
 * picks a default dock). */
bool aos_menu_has_dock(void);

/* Reads menu.txt again. No file is not an error: every app shows in its
 * usual order. Call from the UI task, with the LVGL lock. Returns the number
 * of folders. */
int aos_menu_load(void);

/* The top level, as the launcher shows it: the file's order with the apps
 * that are installed, then every installed app the file does not place. */
int aos_menu_root(aos_menu_item_t *out, int max);

/* A folder's installed apps, in its order. With out NULL, only counts them. */
int aos_menu_folder_apps(int folder, const aos_app_t **out, int max);

int                      aos_menu_folder_count(void);
const aos_menu_folder_t *aos_menu_folder(int index);
int                      aos_menu_folder_find(const char *id);   /* -1 if none */

/* The ids of the file's "hide" lines, installed or not, in file order:
 * NULL past the last. For whoever writes menu.txt back. */
const char *aos_menu_hidden(int i);

/* Checks a menu.txt before it is written, for the portal. Touches no state,
 * so any task may call it. On failure 'err' says which line and why. */
bool aos_menu_validate(const char *text, size_t len, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
