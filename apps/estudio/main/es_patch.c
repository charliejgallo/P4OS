/*
 * Estudio - the synth's parameters and the factory presets.
 *
 * A patch is a flat array of floats in real units (seconds, hertz,
 * semitones), indexed by P_*. ES_PDESC says, for each one, its JSON key, its
 * name, its range and how a slider walks it (linear, exponential, squared,
 * whole numbers, a list of options), so the editor, the JSON and the
 * presets are all driven by the one table.
 */
#include "es.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const O_ENGINE[] = { N_("Sustractivo"), N_("FM"), NULL };
static const char *const O_WAVE[]   = { N_("Seno"), N_("Triáng."), N_("Sierra"), N_("Cuadr."), N_("Ruido"), NULL };
static const char *const O_LWAVE[]  = { N_("Seno"), N_("Triáng."), N_("Sierra"), N_("Cuadr."), N_("Azar"), NULL };
static const char *const O_FTYPE[]  = { "LP 12", "LP 24", "HP", "BP", NULL };
static const char *const O_LDST[]   = { N_("Altura"), N_("Filtro"), N_("Volumen"), N_("Ancho"), NULL };
static const char *const O_MONO[]   = { N_("Poli"), N_("Mono"), NULL };

const es_pdesc_t ES_PDESC[P_COUNT] = {
    [P_ENGINE] = { "motor",      N_("Motor"),            0, 1, 0,       PK_ENUM, U_NONE, O_ENGINE },
    [P_O1W]    = { "o1_onda",    N_("Onda"),             0, 4, W_SAW,   PK_ENUM, U_NONE, O_WAVE },
    [P_O1OCT]  = { "o1_oct",     N_("Octava"),          -2, 2, 0,       PK_INT,  U_OCT,  NULL },
    [P_O1PW]   = { "o1_ancho",   N_("Ancho del pulso"), 0.05f, 0.95f, 0.5f, PK_LIN, U_PCT, NULL },
    [P_O2W]    = { "o2_onda",    N_("Onda"),             0, 4, W_SAW,   PK_ENUM, U_NONE, O_WAVE },
    [P_O2SEMI] = { "o2_semi",    N_("Semitonos"),      -24, 24, 0,      PK_INT,  U_SEMI, NULL },
    [P_O2FINE] = { "o2_fino",    N_("Fino"),           -50, 50, 7,      PK_LIN,  U_CENT, NULL },
    [P_O2MIX]  = { "o2_nivel",   N_("Nivel"),            0, 1, 0.5f,    PK_LIN,  U_PCT,  NULL },
    [P_NOISE]  = { "ruido",      N_("Ruido"),            0, 1, 0,       PK_LIN,  U_PCT,  NULL },
    [P_FTYPE]  = { "filtro",     N_("Tipo"),             0, 3, F_LP24,  PK_ENUM, U_NONE, O_FTYPE },
    [P_CUT]    = { "corte",      N_("Corte"),           30, 16000, 1800, PK_EXP, U_HZ,   NULL },
    [P_RES]    = { "resonancia", N_("Resonancia"),       0, 1, 0.25f,   PK_LIN,  U_PCT,  NULL },
    [P_FENV]   = { "env_filtro", N_("Envolvente"),      -1, 1, 0.35f,   PK_LIN,  U_PCT,  NULL },
    [P_FKEY]   = { "seguimiento", N_("Seguir la nota"),  0, 1, 0.5f,    PK_LIN,  U_PCT,  NULL },
    [P_FVEL]   = { "vel_filtro", N_("Por velocidad"),    0, 1, 0.3f,    PK_LIN,  U_PCT,  NULL },
    [P_A1]     = { "a1",         N_("Ataque"),       0.001f, 5, 0.005f, PK_EXP,  U_SEC,  NULL },
    [P_D1]     = { "d1",         N_("Caída"),        0.005f, 6, 0.4f,   PK_EXP,  U_SEC,  NULL },
    [P_S1]     = { "s1",         N_("Sostén"),           0, 1, 0.7f,    PK_LIN,  U_PCT,  NULL },
    [P_R1]     = { "r1",         N_("Relajación"),   0.005f, 8, 0.25f,  PK_EXP,  U_SEC,  NULL },
    [P_A2]     = { "a2",         N_("Ataque"),       0.001f, 5, 0.005f, PK_EXP,  U_SEC,  NULL },
    [P_D2]     = { "d2",         N_("Caída"),        0.005f, 6, 0.35f,  PK_EXP,  U_SEC,  NULL },
    [P_S2]     = { "s2",         N_("Sostén"),           0, 1, 0.2f,    PK_LIN,  U_PCT,  NULL },
    [P_R2]     = { "r2",         N_("Relajación"),   0.005f, 8, 0.3f,   PK_EXP,  U_SEC,  NULL },
    [P_LFOW]   = { "lfo_onda",   N_("Onda"),             0, 4, W_SINE,  PK_ENUM, U_NONE, O_LWAVE },
    [P_LFOR]   = { "lfo_vel",    N_("Velocidad"),     0.05f, 20, 5,     PK_EXP,  U_HZ,   NULL },
    [P_LFODST] = { "lfo_destino", N_("Destino"),         0, 3, LD_PITCH, PK_ENUM, U_NONE, O_LDST },
    [P_LFODEP] = { "lfo_prof",   N_("Profundidad"),      0, 1, 0,       PK_LIN,  U_PCT,  NULL },
    [P_GLIDE]  = { "glide",      N_("Glide"),            0, 1, 0,       PK_SQ,   U_SEC,  NULL },
    [P_MONO]   = { "mono",       N_("Voces"),            0, 1, 0,       PK_ENUM, U_NONE, O_MONO },
    [P_FMRAT]  = { "fm_rel",     N_("Relación"),      0.25f, 12, 2,     PK_EXP,  U_RATIO, NULL },
    [P_FMIDX]  = { "fm_indice",  N_("Índice"),           0, 10, 2,      PK_LIN,  U_X,    NULL },
    [P_FMENV]  = { "fm_env",     N_("Índice por env. 2"), 0, 10, 3,     PK_LIN,  U_X,    NULL },
    [P_FMFB]   = { "fm_realim",  N_("Realimentación"),   0, 1, 0,       PK_LIN,  U_PCT,  NULL },
    [P_VOL]    = { "volumen",    N_("Volumen"),          0, 1, 0.8f,    PK_LIN,  U_PCT,  NULL },
    [P_VELAMP] = { "vel_volumen", N_("Velocidad"),       0, 1, 0.6f,    PK_LIN,  U_PCT,  NULL },
};

const char *const ES_DRUM_NAMES[DS_COUNT] = {
    N_("Bombo"), N_("Bombo largo"), N_("Redoblante"), N_("Aplauso"), N_("Hi-hat cerrado"),
    N_("Hi-hat abierto"), N_("Tom grave"), N_("Tom agudo"), N_("Platillo"), N_("Cencerro"),
    N_("Clave"), N_("Aro"), N_("Shaker"),
};

const char *const ES_DLY_NAMES[DD_COUNT] = {
    "1/32", "1/16", "1/16.", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2",
};

/* ---- the sliders ---- */

float es_param_to01(int id, float v)
{
    const es_pdesc_t *d = &ES_PDESC[id];
    float t;
    switch (d->kind) {
    case PK_EXP: t = logf(es_clampf(v, d->min, d->max) / d->min) / logf(d->max / d->min); break;
    case PK_SQ:  t = sqrtf(es_clampf((v - d->min) / (d->max - d->min), 0, 1)); break;
    default:     t = (v - d->min) / (d->max - d->min); break;
    }
    return es_clampf(t, 0, 1);
}

float es_param_from01(int id, float t)
{
    const es_pdesc_t *d = &ES_PDESC[id];
    t = es_clampf(t, 0, 1);
    switch (d->kind) {
    case PK_EXP:  return d->min * expf(t * logf(d->max / d->min));
    case PK_SQ:   return d->min + (d->max - d->min) * t * t;
    case PK_INT:
    case PK_ENUM: return roundf(d->min + (d->max - d->min) * t);
    default:      return d->min + (d->max - d->min) * t;
    }
}

/* a decimal point in English, a comma in Spanish and German */
static void decimal(char *s)
{
    const char *l = aos_i18n_current();
    if (l && l[0] == 'e' && l[1] == 'n') return;
    for (; *s; s++) if (*s == '.') *s = ',';
}

void es_param_format(int id, float v, char *out, size_t n)
{
    const es_pdesc_t *d = &ES_PDESC[id];
    if (d->kind == PK_ENUM) {
        int i = (int)roundf(v);
        snprintf(out, n, "%s", d->opts ? _(d->opts[i < 0 ? 0 : i]) : "");
        return;
    }
    switch (d->unit) {
    case U_HZ:
        if (v < 10.0f) snprintf(out, n, "%.2f Hz", (double)v);
        else if (v < 1000.0f) snprintf(out, n, "%d Hz", (int)roundf(v));
        else snprintf(out, n, "%.1f kHz", (double)(v / 1000.0f));
        break;
    case U_SEC:
        if (v < 0.001f) snprintf(out, n, "0");
        else if (v < 1.0f) snprintf(out, n, "%d ms", (int)roundf(v * 1000.0f));
        else snprintf(out, n, "%.2f s", (double)v);
        break;
    case U_PCT:  snprintf(out, n, d->min < 0 ? "%+d %%" : "%d %%", (int)roundf(v * 100.0f)); break;
    case U_SEMI: snprintf(out, n, "%+d", (int)roundf(v)); break;
    case U_CENT: snprintf(out, n, "%+d ct", (int)roundf(v)); break;
    case U_OCT:  snprintf(out, n, "%+d", (int)roundf(v)); break;
    case U_RATIO: snprintf(out, n, "%.2f", (double)v); break;
    case U_X:    snprintf(out, n, "%.1f", (double)v); break;
    default:     snprintf(out, n, "%.2f", (double)v); break;
    }
    decimal(out);
}

/* ---- presets ---- */

void es_patch_default(es_patch_t *p)
{
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", "Básico");
    for (int i = 0; i < P_COUNT; i++) p->p[i] = ES_PDESC[i].def;
}

typedef struct { int8_t id; float v; } kv_t;
typedef struct { const char *name; kv_t kv[24]; } preset_t;

#define END { -1, 0 }

static const preset_t PRESETS[] = {
    { N_("Básico"), { END } },
    { N_("Bajo redondo"), {
        { P_O1OCT, -1 }, { P_O2W, W_SQUARE }, { P_O2SEMI, -12 }, { P_O2FINE, 0 }, { P_O2MIX, 0.5f },
        { P_CUT, 220 }, { P_RES, 0.35f }, { P_FENV, 0.45f }, { P_FKEY, 0.3f },
        { P_A1, 0.002f }, { P_D1, 0.3f }, { P_S1, 0.8f }, { P_R1, 0.08f },
        { P_A2, 0.001f }, { P_D2, 0.22f }, { P_S2, 0 }, { P_R2, 0.1f },
        { P_MONO, 1 }, { P_GLIDE, 0.04f }, END } },
    { N_("Bajo ácido"), {
        { P_O1OCT, -1 }, { P_O2MIX, 0 }, { P_CUT, 260 }, { P_RES, 0.84f }, { P_FENV, 0.62f },
        { P_FVEL, 0.7f }, { P_FKEY, 0.2f }, { P_A1, 0.001f }, { P_D1, 0.25f }, { P_S1, 0.9f },
        { P_R1, 0.05f }, { P_A2, 0.001f }, { P_D2, 0.18f }, { P_S2, 0 }, { P_R2, 0.08f },
        { P_MONO, 1 }, { P_GLIDE, 0.06f }, END } },
    { N_("Lead sierra"), {
        { P_O2FINE, 9 }, { P_O2MIX, 0.7f }, { P_FTYPE, F_LP12 }, { P_CUT, 2600 }, { P_RES, 0.3f },
        { P_FENV, 0.25f }, { P_A1, 0.005f }, { P_D1, 0.3f }, { P_S1, 0.85f }, { P_R1, 0.2f },
        { P_LFOR, 5.5f }, { P_LFODEP, 0.25f }, { P_MONO, 1 }, { P_GLIDE, 0.05f }, END } },
    { N_("Lead cuadrado"), {
        { P_O1W, W_SQUARE }, { P_O1PW, 0.35f }, { P_O2W, W_SQUARE }, { P_O2SEMI, 12 }, { P_O2FINE, 0 },
        { P_O2MIX, 0.25f }, { P_FTYPE, F_LP12 }, { P_CUT, 3500 }, { P_RES, 0.2f }, { P_FENV, 0.15f },
        { P_LFODST, LD_PW }, { P_LFOR, 0.8f }, { P_LFODEP, 0.5f }, { P_MONO, 1 }, { P_GLIDE, 0.03f },
        { P_S1, 0.9f }, END } },
    { N_("Pad cálido"), {
        { P_O2FINE, 12 }, { P_O2MIX, 0.9f }, { P_FTYPE, F_LP12 }, { P_CUT, 900 }, { P_RES, 0.15f },
        { P_FENV, 0.3f }, { P_A1, 0.8f }, { P_D1, 1.5f }, { P_S1, 0.8f }, { P_R1, 1.6f },
        { P_A2, 1.2f }, { P_D2, 2.0f }, { P_S2, 0.5f }, { P_R2, 2.0f },
        { P_LFODST, LD_CUT }, { P_LFOR, 0.3f }, { P_LFODEP, 0.3f }, { P_VOL, 0.7f }, END } },
    { N_("Cuerdas"), {
        { P_O2FINE, -10 }, { P_O2MIX, 0.8f }, { P_CUT, 2400 }, { P_RES, 0.1f }, { P_FENV, 0.1f },
        { P_A1, 0.45f }, { P_D1, 1.0f }, { P_S1, 0.85f }, { P_R1, 1.2f }, { P_FVEL, 0.2f },
        { P_LFOR, 5.0f }, { P_LFODEP, 0.12f }, { P_VOL, 0.7f }, END } },
    { N_("Pluck"), {
        { P_O2W, W_SQUARE }, { P_O2SEMI, 12 }, { P_O2FINE, 0 }, { P_O2MIX, 0.3f }, { P_CUT, 400 },
        { P_RES, 0.3f }, { P_FENV, 0.7f }, { P_A2, 0.001f }, { P_D2, 0.18f }, { P_S2, 0 },
        { P_A1, 0.001f }, { P_D1, 0.9f }, { P_S1, 0 }, { P_R1, 0.35f }, END } },
    { N_("Órgano"), {
        { P_O1W, W_SINE }, { P_O2W, W_SINE }, { P_O2SEMI, 12 }, { P_O2FINE, 0 }, { P_O2MIX, 0.7f },
        { P_NOISE, 0.02f }, { P_FTYPE, F_LP12 }, { P_CUT, 8000 }, { P_RES, 0 }, { P_FENV, 0 },
        { P_A1, 0.004f }, { P_D1, 0.1f }, { P_S1, 1 }, { P_R1, 0.06f }, { P_VELAMP, 0.1f },
        { P_LFODST, LD_AMP }, { P_LFOR, 6.0f }, { P_LFODEP, 0.15f }, END } },
    { N_("Bronces"), {
        { P_O2FINE, 6 }, { P_O2MIX, 0.8f }, { P_FTYPE, F_LP12 }, { P_CUT, 700 }, { P_RES, 0.1f },
        { P_FENV, 0.55f }, { P_A2, 0.06f }, { P_D2, 0.4f }, { P_S2, 0.4f }, { P_R2, 0.3f },
        { P_A1, 0.04f }, { P_D1, 0.3f }, { P_S1, 0.8f }, { P_R1, 0.2f }, END } },
    { N_("Sub grave"), {
        { P_O1W, W_SINE }, { P_O1OCT, -1 }, { P_O2W, W_TRI }, { P_O2FINE, 0 }, { P_O2MIX, 0.3f },
        { P_FTYPE, F_LP12 }, { P_CUT, 400 }, { P_RES, 0 }, { P_FENV, 0.1f }, { P_MONO, 1 },
        { P_A1, 0.005f }, { P_S1, 1 }, { P_R1, 0.1f }, END } },
    { N_("Campana FM"), {
        { P_ENGINE, 1 }, { P_FMRAT, 3.5f }, { P_FMIDX, 1.5f }, { P_FMENV, 4 }, { P_FTYPE, F_LP12 },
        { P_CUT, 12000 }, { P_RES, 0 }, { P_FENV, 0 }, { P_A2, 0.001f }, { P_D2, 1.2f }, { P_S2, 0 },
        { P_R2, 1.2f }, { P_A1, 0.001f }, { P_D1, 2.5f }, { P_S1, 0 }, { P_R1, 2.0f }, END } },
    { N_("Piano eléctrico"), {
        { P_ENGINE, 1 }, { P_FMRAT, 1 }, { P_FMIDX, 1 }, { P_FMENV, 3.5f }, { P_FTYPE, F_LP12 },
        { P_CUT, 6000 }, { P_RES, 0 }, { P_FENV, 0 }, { P_A2, 0.001f }, { P_D2, 0.5f }, { P_S2, 0.1f },
        { P_A1, 0.001f }, { P_D1, 1.6f }, { P_S1, 0.2f }, { P_R1, 0.5f }, { P_VELAMP, 0.8f },
        { P_LFODST, LD_AMP }, { P_LFOR, 4.0f }, { P_LFODEP, 0.1f }, END } },
    { N_("Bajo FM"), {
        { P_ENGINE, 1 }, { P_O1OCT, -1 }, { P_FMRAT, 1 }, { P_FMIDX, 2.5f }, { P_FMENV, 3 },
        { P_FMFB, 0.2f }, { P_CUT, 3000 }, { P_FENV, 0 }, { P_A2, 0.001f }, { P_D2, 0.15f },
        { P_S2, 0.2f }, { P_A1, 0.001f }, { P_D1, 0.5f }, { P_S1, 0.6f }, { P_R1, 0.1f },
        { P_MONO, 1 }, END } },
};

int es_patch_factory_n(void)
{
    return (int)(sizeof PRESETS / sizeof PRESETS[0]);
}

const es_patch_t *es_patch_factory(int i)
{
    static es_patch_t p;
    if (i < 0 || i >= es_patch_factory_n()) i = 0;
    es_patch_default(&p);
    snprintf(p.name, sizeof p.name, "%s", PRESETS[i].name);
    for (const kv_t *k = PRESETS[i].kv; k->id >= 0; k++) p.p[k->id] = k->v;
    return &p;
}

/* ---- the drums and MIDI ---- */

int es_drum_gm(int src)
{
    static const uint8_t GM[DS_COUNT] = { 36, 35, 38, 39, 42, 46, 45, 48, 49, 56, 75, 37, 70 };
    return src >= 0 && src < DS_COUNT ? GM[src] : 60;
}

int es_drum_from_gm(const es_proj_t *p, int note)
{
    for (int t = 0; t < ES_DRUMS; t++)
        if (p->dt[t].src < DS_COUNT && es_drum_gm(p->dt[t].src) == note) return t;
    /* the usual neighbours: a second kick, the other snare, the other hats */
    static const struct { uint8_t gm, as; } NEAR[] = {
        { 35, 36 }, { 36, 35 }, { 40, 38 }, { 37, 38 }, { 44, 42 }, { 41, 45 }, { 43, 45 },
        { 47, 48 }, { 50, 48 }, { 57, 49 }, { 51, 49 }, { 52, 49 }, { 55, 49 },
    };
    for (size_t i = 0; i < sizeof NEAR / sizeof NEAR[0]; i++)
        if (NEAR[i].gm == note)
            for (int t = 0; t < ES_DRUMS; t++)
                if (p->dt[t].src < DS_COUNT && es_drum_gm(p->dt[t].src) == NEAR[i].as) return t;
    if (note >= 60 && note < 60 + ES_DRUMS) return note - 60;       /* the samples' place */
    return -1;
}

const char *es_note_name(int note, char *buf, size_t n)
{
    static const char *const N[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    snprintf(buf, n, "%s%d", N[note % 12], note / 12 - 1);
    return buf;
}
