/*
 * P4OS - Modbus: device profiles (mb_prof.c).
 *
 * A profile says what a device has and where: its unit, speed and format,
 * and a list of points - a name, the table and address, how the bytes read
 * (type, scale, unit), whether it can be written, and, for when the app
 * plays the device, how the value moves. One CSV file per device in
 * <card>/modbus/perfiles/, which a spreadsheet opens, the portal's page
 * edits, and the app edits too:
 *
 *   # P4OS Modbus profile
 *   nombre,Multisensor
 *   nota,Temperatura humedad CO2 y luz
 *   esclavo,1
 *   velocidad,9600
 *   formato,8N1
 *   periodo,1000
 *   # tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor
 *   ir,0,Temperatura,s16,0.1,°C,r,15,30,seno,22
 *   coil,0,Relé,bit,,,rw,,,,0
 *
 * Tables: coil, di, hr, ir. Types: bit, u16, s16, hex, u32, s32, f32 (the
 * high word first, ABCD) and u32cdab, s32cdab, f32cdab (the low word
 * first). The value is the raw number times the scale. Access r or rw.
 * Simulations: fijo, seno, rampa, deriva, contador, alterna, azar, moving
 * between min and max; valor is where it starts. Lines starting with # are
 * comments; a number is decimal with a point, or 0x hex.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_PTS 64

enum { MB_TAB_COIL = 1, MB_TAB_DI = 2, MB_TAB_HR = 3, MB_TAB_IR = 4 };
enum { MB_T_BIT = 0, MB_T_U16, MB_T_S16, MB_T_HEX, MB_T_U32, MB_T_S32, MB_T_F32, MB_T_U32R, MB_T_S32R, MB_T_F32R, MB_T_COUNT };
enum { MB_SIM_NONE = 0, MB_SIM_SINE, MB_SIM_RAMP, MB_SIM_DRIFT, MB_SIM_COUNTER, MB_SIM_TOGGLE, MB_SIM_RANDOM, MB_SIM_COUNT };

typedef struct {
    uint8_t  table;             /* MB_TAB_* */
    uint8_t  type;              /* MB_T_* */
    uint8_t  sim;               /* MB_SIM_* */
    bool     rw;
    uint16_t addr;
    float    scale;             /* value = raw x scale (0 is taken as 1) */
    float    min, max, value;
    char     name[32];
    char     unit[12];
} mb_point_t;

typedef struct {
    char     file[48];          /* "multisensor.csv" */
    char     name[48];
    char     note[96];
    uint8_t  unit;              /* the slave's address */
    uint32_t baud;              /* 0: not said */
    char     par;               /* 0: not said */
    uint8_t  stop;
    uint16_t poll_ms;
    int      n;
    mb_point_t pt[MB_PTS];
} mb_prof_t;

extern const char *const MB_TYPE_NAME[MB_T_COUNT];
extern const char *const MB_SIM_NAME[MB_SIM_COUNT];
extern const char *const MB_TABLE_NAME[5];

const char *mb_prof_dir(void);              /* <card>/modbus/perfiles, made if missing */
/* The files there, sorted: up to max names of up to 48 bytes. */
int  mb_prof_list(char (*names)[48], int max);
bool mb_prof_load(const char *file, mb_prof_t *p, char *err, size_t err_len);
bool mb_prof_save(const mb_prof_t *p);
bool mb_prof_delete(const char *file);
/* The examples, written when the folder has no profile at all. */
void mb_prof_examples(void);
/* A file name for a new profile called this ("Mi sensor" -> "mi-sensor.csv"). */
void mb_prof_file_for(const char *name, char *out, size_t n);
uint32_t mb_prof_mtime(const char *file);

/* 1 or 2 registers (a bit counts 1). */
int    mb_pt_words(const mb_point_t *pt);
double mb_pt_decode(const mb_point_t *pt, const uint16_t *raw);
void   mb_pt_encode(const mb_point_t *pt, double v, uint16_t *raw);
/* "23,4 °C", "ON", "0x1F00" */
void   mb_pt_format(const mb_point_t *pt, double v, char *out, size_t n);
int    mb_pt_decimals(const mb_point_t *pt);
/* Where a simulated point is at time t (seconds), from its last value. */
double mb_pt_sim(const mb_point_t *pt, double last, float t, float dt);

/* The reads that cover a profile's points, joining neighbours: up to max
 * blocks of (table, addr, count); each point's block in blk_of[]. */
typedef struct { uint8_t table; uint16_t addr, n; } mb_block_t;
int mb_prof_blocks(const mb_prof_t *p, mb_block_t *b, int max, int *blk_of);
