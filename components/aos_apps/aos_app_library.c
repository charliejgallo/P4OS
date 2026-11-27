/*
 * P4OS - the App Library's categories (aos_ui_set_app_category).
 *
 * An app from the Store is in the category its catalogue gives it
 * (aos_store_app_t.cat); the built-in apps, which the catalogue does not
 * list, by the table below; any other game ("demo.*") with the games, and
 * the rest in "utiles". The shell cannot see the Store, so it asks here.
 */
#include "aos_ui.h"
#include "aos_store.h"

#include <string.h>

static const struct { const char *id, *cat; } BUILTIN[] = {
    { "aos.settings", "sistema" }, { "aos.store", "sistema" },  { "aos.sysmon", "sistema" },
    { "aos.modules", "sistema" },  { "aos.files", "utiles" },   { "aos.clock", "utiles" },
    { "aos.calendar", "utiles" },  { "aos.calc", "utiles" },    { "aos.convert", "utiles" },
    { "aos.macropad", "utiles" },  { "aos.music", "medios" },   { "aos.photos", "medios" },
    { "aos.ha", "red" },           { "aos.mqtt", "red" },       { "aos.bot", "red" },
    { "aos.net", "red" },          { "aos.zigbee", "red" },     { "aos.claude", "red" },
    { "aos.bus", "taller" },       { "aos.flasher", "taller" }, { "aos.bench", "taller" },
    { "aos.serial", "taller" },    { "aos.elec", "taller" },    { "aos.leds", "taller" },
    { "aos.life", "juegos" },
};

/* The catalogue's word, as one of the shell's: a pointer that stays */
static const char *const KNOWN[] = { "juegos", "medios", "red", "taller", "crear", "programar", "utiles", "sistema" };

static const char *category(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++)
        if (!strcmp(BUILTIN[i].id, id)) return BUILTIN[i].cat;
    aos_store_app_t a;
    int n = aos_store_count();
    for (int i = 0; i < n; i++) {
        if (!aos_store_app(i, &a) || a.system || strcmp(a.id, id)) continue;
        for (size_t k = 0; k < sizeof KNOWN / sizeof KNOWN[0]; k++)
            if (!strcmp(KNOWN[k], a.cat)) return KNOWN[k];
        break;
    }
    return !strncmp(id, "demo.", 5) ? "juegos" : "utiles";
}

void aos_app_library_register(void)
{
    aos_ui_set_app_category(category);
}
