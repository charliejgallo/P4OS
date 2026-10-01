/*
 * P4OS - modules.txt as a text: read it, check it, save it (aos_modules.c).
 *
 * aos_io_validate() says whether aos_io_load() will take a file. This adds
 * what a person editing it wants to hear before saving, for the Módulos app
 * and the portal alike: the line and the reason as a code each UI words in
 * its language, and the warnings that do not stop a load but will stop the
 * module working (a GPIO in two ports, a module on a port that does not
 * exist, a sensor on a port that is not I2C).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_MOD_OK = 0,
    /* errors: aos_io_load() would not take the file */
    AOS_MOD_E_LONG,             /* a line too long */
    AOS_MOD_E_SYNTAX,           /* not "port|module <name> ..." */
    AOS_MOD_E_TOO_MANY_PORTS,
    AOS_MOD_E_PORT_NAME,        /* a port name too long */
    AOS_MOD_E_PORT_KIND,        /* a port name not starting with uart, i2c, spi or gpio */
    AOS_MOD_E_NOT_HEADER,       /* a GPIO that is not on the header */
    AOS_MOD_E_RESERVED,         /* a reserved pin (console, BOOT) */
    AOS_MOD_E_MISSING_PIN,
    AOS_MOD_E_DUP_PORT,         /* two ports with one name (a = the name) */
    AOS_MOD_E_TOO_MANY_MODULES,
    AOS_MOD_E_MODULE_PORT,      /* a module line without a port */
    AOS_MOD_E_UNKNOWN,          /* neither port nor module */
    AOS_MOD_E_OTHER,            /* raw has aos_io's own words */
    /* warnings: it loads, but something will not work */
    AOS_MOD_W_PIN_TWICE,        /* gpio is in ports a and b */
    AOS_MOD_W_NO_PORT,          /* module a is on port b, which is not declared */
    AOS_MOD_W_NOT_I2C,          /* sensor module a is on b, which is not an I2C port */
} aos_mod_code_t;

typedef struct {
    int  code;                  /* aos_mod_code_t */
    int  line;                  /* 1-based, 0 when it is not about one line */
    int  gpio;
    char a[24], b[24];
    char raw[64];               /* aos_io's message, for AOS_MOD_E_OTHER */
} aos_mod_msg_t;

#define AOS_MOD_WARN_MAX 4

typedef struct {
    bool ok;                    /* aos_io_load() will take it */
    int  ports, modules;
    aos_mod_msg_t err;
    int  nwarn;
    aos_mod_msg_t warn[AOS_MOD_WARN_MAX];
} aos_mod_check_t;

void aos_modules_check(const char *text, size_t len, aos_mod_check_t *out);

/* The text to edit, malloc'd (free it): the card's modules.txt, or, without
 * one, what is loaded now written out as a modules.txt (the default
 * profile). *from_file says which. NULL only when out of memory. */
char *aos_modules_read(bool *from_file);

/* Checks, writes <card>/modules.txt, reloads it (aos_io_load) and has the
 * sensors look again. false: it did not pass the check (out says why), or
 * there is no card to write to (out->err.code stays AOS_MOD_OK). */
bool aos_modules_save(const char *text, size_t len, aos_mod_check_t *out);

/* aos_modules_read() + one more line at the end + aos_modules_save(). */
bool aos_modules_append(const char *line, aos_mod_check_t *out);

/* A module name not used yet: base, or base_2, base_3... */
void aos_modules_free_name(const char *base, char *out, size_t n);

#ifdef __cplusplus
}
#endif
