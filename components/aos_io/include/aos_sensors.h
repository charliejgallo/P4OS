/*
 * P4OS - I2C sensors on the header: drivers, autodetect and the service
 * that keeps their latest readings (aos_sensors.c, docs/MODULES.md).
 *
 * A thread reads every sensor about once a second and keeps, per sensor, its
 * last values and a five-minute history. The sensors come from two places:
 *
 *   declared   a "module" line of modules.txt whose name (or chip=) is a
 *              chip with a driver: "module bme280 i2c.ext addr=0x76",
 *              "module banco ina219 i2c.ext addr=0x40 shunt=100"...
 *   detected   the I2C ports other than the board's own are probed at the
 *              addresses the supported chips use, and a chip that proves
 *              what it is (a chip id, a manufacturer id, a CRC, registers
 *              only it has at power-on) is read without being declared.
 *              One that cannot prove it (a BH1750 looks like any PCF8574)
 *              is only a CANDIDATE: the UI offers to declare it.
 *
 * The service opens a port only for the few milliseconds of a round and
 * closes it again, so the Bus app (or anybody) can use the same port in
 * between; when the pins are someone else's, the round is skipped.
 *
 * The same code runs in the simulator, where sim/sensors_sim.c answers on
 * i2c.ext as a BME280, an SHT31, a BH1750, an INA219 and an ADS1115
 * (P4_SIM_SENSORS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a value measures; each has one unit, always the same. */
typedef enum {
    AOS_SQ_TEMP = 0,        /* °C  */
    AOS_SQ_HUM,             /* %RH */
    AOS_SQ_PRESS,           /* hPa */
    AOS_SQ_LUX,             /* lx  */
    AOS_SQ_VOLT,            /* V   */
    AOS_SQ_CURR,            /* A   */
    AOS_SQ_POWER,           /* W   */
    AOS_SQ_COUNT
} aos_sensor_q_t;

typedef enum {
    AOS_SENSOR_WAIT = 0,    /* not read yet */
    AOS_SENSOR_OK,
    AOS_SENSOR_ERR,         /* the last read failed: err says why */
    AOS_SENSOR_BUSY,        /* the port's pins were someone else's this round */
} aos_sensor_state_t;

#define AOS_SENSOR_MAX      12
#define AOS_SENSOR_VALUES   4
#define AOS_SENSOR_HIST     300     /* samples of history, one a second */
#define AOS_SENSOR_CAND_MAX 12

typedef struct {
    char    key[12];                /* "temp", "hum", "press", "lux", "bus", "current", "power", "a0".."a3" */
    uint8_t q;                      /* aos_sensor_q_t */
    float   v;
} aos_sensor_value_t;

typedef struct {
    char     name[24];              /* the module's name, or the chip's for a detected one */
    char     chip[12];              /* "BME280" */
    char     port[16];
    uint8_t  addr;
    bool     declared;              /* from modules.txt */
    uint8_t  state;                 /* aos_sensor_state_t */
    /* why it failed, "<why>" or "<why>:<detail>": nack (no answer), crc,
     * id:<what the chip said it is>, notready, busy:<owner of the pins>,
     * port (cannot open it), board:<the board's chip at that address> */
    char     err[48];
    uint32_t t_ms;                  /* uptime of the last good read */
    uint32_t reads, errors;
    int      n;
    aos_sensor_value_t v[AOS_SENSOR_VALUES];
} aos_sensor_t;

/* Something answered where a supported chip lives, but did not prove it is
 * that chip: the UI offers "module <chip> <port> addr=<addr>". */
typedef struct {
    char    port[16];
    uint8_t addr;
    char    chip[12];               /* the driver it would get ("bh1750") */
    char    guess[40];              /* aos_io_i2c_guess() */
} aos_sensor_cand_t;

/* The chips with a driver, for forms ("Agregar módulo"). */
typedef struct {
    const char *id;                 /* "bme280": the module name in modules.txt */
    const char *name;               /* "BME280" */
    uint8_t     addrs[4];           /* the addresses it can have, the usual one first */
    uint8_t     naddr;
    bool        shunt;              /* takes shunt= (milliohms) */
    bool        gain;               /* takes gain= (full scale in mV) */
} aos_sensor_chip_t;

int                      aos_sensor_chip_count(void);
const aos_sensor_chip_t *aos_sensor_chip_at(int i);
const aos_sensor_chip_t *aos_sensor_chip_find(const char *id);   /* also "sht31", "aht10"... */

/* Starts the service (once; later calls do nothing). Any of the calls below
 * starts it too. */
void aos_sensors_start(void);
/* Starts it only if modules.txt declares a sensor: for boot. */
void aos_sensors_autostart(void);
/* Somebody is looking (an app, the portal): autodetect every 5 s instead of
 * every minute, for the next 10 s. */
void aos_sensors_keep(void);
/* Forget the detected ones and probe again at the next round (after an
 * edit of modules.txt, or a "Buscar"). */
void aos_sensors_rescan(void);
/* Bumped at the end of every round: a UI redraws when it moves. */
uint32_t aos_sensors_seq(void);
/* How the last round went on each I2C port it looked at. */
typedef struct {
    char     port[16];
    uint8_t  state;                 /* OK, BUSY (owner has the pins) or ERR */
    char     owner[24];
    int      found;                 /* sensors on it */
    bool     scanned;               /* autodetect ran on it at least once */
    uint32_t scan_ms;               /* uptime of the last autodetect */
} aos_sensor_port_t;
int aos_sensors_ports(aos_sensor_port_t *out, int max);

int  aos_sensor_count(void);
bool aos_sensor_at(int i, aos_sensor_t *out);                /* a copy */
/* The latest value of one sensor by name and key: ("bme280", "temp").
 * false while it has none (not read yet, or failing). */
bool aos_sensor_value(const char *name, const char *key, float *out);
/* Value k of sensor i, oldest first; returns how many (up to max). */
int  aos_sensor_history(int i, int k, float *out, int max);
int  aos_sensor_candidates(aos_sensor_cand_t *out, int max);

const char *aos_sensor_unit(int q);                          /* "°C", "hPa"... */

#ifdef __cplusplus
}
#endif
