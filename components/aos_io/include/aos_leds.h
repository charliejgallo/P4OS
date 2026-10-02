/*
 * P4OS - the LED strip service (aos_leds.c): one addressable strip on any
 * usable GPIO of the header, driven by effects in the background the way
 * WLED does it, whatever app is in front.
 *
 * The app (Tiras LED) and later the portal only read and write the
 * configuration; the service opens the strip, runs the effect at up to
 * 50 fps, and limits the brightness so that the estimated current stays
 * within the supply (WLED's ABL). The configuration is kept in the
 * preferences and comes back at boot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aos_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_FX_SOLID = 0, AOS_FX_BLINK, AOS_FX_BREATHE, AOS_FX_WIPE, AOS_FX_RAINBOW, AOS_FX_RAINBOW_CYCLE,
    AOS_FX_THEATER, AOS_FX_SCANNER, AOS_FX_TWINKLE, AOS_FX_FIRE, AOS_FX_METEOR, AOS_FX_GRADIENT,
    AOS_FX_RUNNING, AOS_FX_COUNT
} aos_leds_fx_t;

typedef struct {
    int8_t            gpio;         /* -1: no strip set up */
    aos_strip_type_t  type;
    aos_strip_order_t order;
    uint16_t          count;
    uint16_t          ma_per_led;   /* at full white, all channels at 255 (WLED: 55 for a WS2812B) */
    uint16_t          psu_ma;       /* what the supply gives the strip; 0: no limit */
    bool              on;
    uint8_t           brightness;   /* 0..255, before the limit */
    uint8_t           effect;       /* aos_leds_fx_t */
    uint8_t           speed;        /* 0..255 */
    uint8_t           intensity;    /* 0..255: the effect's width, density, cooling... */
    uint32_t          color[2];     /* 0xRRGGBB: the main colour and the second one */
    uint8_t           white;        /* RGBW strips: the W channel added to the colours */
    bool              reverse;      /* the effects run from the far end */
} aos_leds_cfg_t;

typedef struct {
    bool     running;               /* the strip is open and being sent */
    int      fps;
    uint32_t ma_estimate;           /* what this frame draws, at the brightness asked for */
    uint32_t ma_out;                /* after the limit */
    uint8_t  brightness_out;        /* the brightness actually sent */
    bool     limited;
    char     error[64];             /* why it is not running, "" when it is or is off */
} aos_leds_status_t;

void aos_leds_get(aos_leds_cfg_t *out);
/* Applies at once; the preferences are written a couple of seconds after
 * the last change, so a slider being dragged does not wear the flash. */
void aos_leds_set(const aos_leds_cfg_t *cfg);
void aos_leds_status(aos_leds_status_t *out);
/* The frame being sent, R G B W a LED, after the brightness and the limit;
 * how many LEDs were copied. For the app's preview. */
int  aos_leds_frame(uint8_t *rgbw, int max_leds);
int  aos_leds_fx_count(void);                  /* the app names them */
void aos_leds_autostart(void);                 /* from the apps' service tick: a strip that was on */

#ifdef __cplusplus
}
#endif
