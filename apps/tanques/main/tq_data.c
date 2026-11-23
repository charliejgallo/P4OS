/*
 * TANQUES - the game's tables (see tq_data.h)
 */
#include "tq_data.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Tanks
 * -------------------------------------------------------------------------- */

const tq_tank_type_t TQ_TANK[TQ_TANK_TYPES] = {
    /* key      name            role                                                   price  hp arm ap  m/AP climb sl  pivot x,z     muz   eye x, y, z            pitch body */
    { "bombo",  N_("Bombo"),  N_("Para todo: buen blindaje, buen motor, tres armas"),     0, 100,  5, 10, 1.6f, 2.2f, 3, 0.64f, 1.38f, 1.40f, -0.75f, 0.52f, 2.42f,  8, 1.45f },
    { "pulga",  N_("Pulga"),  N_("Liviana y rápida: llega lejos, aguanta poco"),        800,  70,  0, 10, 2.3f, 1.6f, 2, 0.40f, 1.02f, 1.10f, -0.80f, 0.40f, 1.92f,  8, 1.25f },
    { "mamut",  N_("Mamut"),  N_("Pesado: mucho blindaje, dos cañones, lento"),        1800, 160, 20,  9, 1.1f, 3.0f, 4, 0.82f, 1.62f, 1.55f, -1.20f, 0.72f, 2.80f,  6, 1.75f },
    { "obus",   N_("Obús"),   N_("Artillería: lo que va por arriba pega más"),         1200,  90,  5, 10, 1.3f, 2.6f, 3, 0.32f, 1.48f, 1.30f, -1.10f, 0.62f, 2.40f, 32, 1.60f },
    { "avispa", N_("Avispa"), N_("Misiles: los guiados y los láser pegan más"),        1400,  85,  0, 11, 1.9f, 2.0f, 3, 0.05f, 1.46f, 0.85f, -1.55f, 0.78f, 2.60f,  6, 1.40f },
    { "topo",   N_("Topo"),   N_("Ingeniero: cava, arregla y planta minas mejor"),     1000, 110, 10, 10, 1.5f, 1.8f, 3, 0.40f, 1.30f, 0.86f, -0.95f, 0.50f, 2.20f,  8, 1.45f },
};

const char *const TQ_UP_NAME[TQ_UPGRADES] = { N_("Blindaje"), N_("Motor"), N_("Velocidad"), N_("Energía") };
const char *const TQ_UP_INFO[TQ_UPGRADES] = {
    N_("Más vida y menos daño por golpe"),
    N_("Sube mejor las lomas"),
    N_("Más metros por punto de acción"),
    N_("Un punto de acción más por turno"),
};

int tq_up_price(int type, int track, int level)
{
    static const int BASE[TQ_UP_MAX + 1] = { 0, 150, 300, 500, 800 };
    (void)track;
    int p = BASE[level < 0 ? 0 : (level > TQ_UP_MAX ? TQ_UP_MAX : level)];
    if (type == TQ_T_MAMUT) p = p * 5 / 4;
    return p;
}

void tq_stats(int type, const uint8_t up[TQ_UPGRADES], tq_stats_t *o)
{
    const tq_tank_type_t *t = &TQ_TANK[type];
    o->hpmax = t->hp + t->hp * 12 * up[TQ_UP_ARMOR] / 100;
    o->armor = t->armor + 4 * up[TQ_UP_ARMOR];
    o->climb = t->climb * (1.0f - 0.12f * up[TQ_UP_ENGINE]);
    o->max_slope = 0.85f + 0.08f * up[TQ_UP_ENGINE];
    o->m_per_ap = t->m_per_ap * (1.0f + 0.10f * up[TQ_UP_SPEED]);
    o->apmax = t->ap + up[TQ_UP_ENERGY];
}

/* --------------------------------------------------------------------------
 * Weapons
 * -------------------------------------------------------------------------- */

const tq_weapon_t TQ_WEAPON[TQ_WEAPONS] = {
    /* name                 info                                                   ap price fly            fx              speed grav radius dmg crater   uses  pitch       colour */
    { N_("Cañón"),          N_("Rápido y recto: el de siempre"),                   3,    0, TQ_FLY_SHELL,  TQ_FX_BLAST,    23, 1.0f, 2.2f, 30, 1.3f, 0.45f, 0, -5, 45, 0xE8572E },
    { N_("Mortero"),        N_("Por arriba de lo que tapa"),                       5,    0, TQ_FLY_LOB,    TQ_FX_BLAST,    15, 1.0f, 3.0f, 38, 1.8f, 0.70f, 0, 30, 75, 0x2E6FB0 },
    { N_("Misil guiado"),   N_("Se tuerce hacia el tanque más cercano"),           6,  600, TQ_FLY_GUIDED, TQ_FX_BLAST,    16, 0.35f,2.4f, 34, 1.4f, 0.50f, 3, -5, 45, 0xD8443B },
    { N_("Racimo"),         N_("Se abre arriba en cinco bombitas"),                6,  700, TQ_FLY_LOB,    TQ_FX_CLUSTER,  15, 1.0f, 1.7f, 16, 1.0f, 0.35f, 2, 25, 75, 0x9B6BDB },
    { N_("Láser"),          N_("Derecho, sin caer, corta lo que encuentra"),       5,  800, TQ_FLY_BEAM,   TQ_FX_BLAST,     0, 0.0f, 0.9f, 28, 0.5f, 0.20f, 0, -5, 20, 0xFF3E7A },
    { N_("Napalm"),         N_("Deja fuego dos turnos"),                           6,  650, TQ_FLY_LOB,    TQ_FX_NAPALM,   15, 1.0f, 3.0f, 18, 1.0f, 0.25f, 2, 25, 75, 0xFF8A1E },
    { N_("Minas"),          N_("Tres minas: explotan al pasar"),                   3,  500, TQ_FLY_LOB,    TQ_FX_MINES,    13, 1.0f, 1.6f, 30, 1.0f, 0.35f, 2, 20, 75, 0x6B7380 },
    { N_("Pulso EMP"),      N_("Sin daño: el de al lado pierde la mitad de AP"),   5,  600, TQ_FLY_LOB,    TQ_FX_EMP,      15, 1.0f, 3.5f,  0, 0.0f, 0.00f, 2, 20, 75, 0x5CC8E8 },
    { N_("Escudo"),         N_("Frena los próximos 40 de daño"),                   4,  550, TQ_FLY_SELF,   TQ_FX_SHIELD,    0, 0.0f, 0.0f, 40, 0.0f, 0.00f, 2,  0,  0, 0x46C8FF },
    { N_("Reparación"),     N_("Recupera 30 de vida"),                             4,  450, TQ_FLY_SELF,   TQ_FX_REPAIR,    0, 0.0f, 0.0f, 30, 0.0f, 0.00f, 2,  0,  0, 0x5ED16A },
    { N_("Humo"),           N_("Una nube: lo que tira desde ahí o hacia ahí se desvía"), 3, 300, TQ_FLY_LOB, TQ_FX_SMOKE,   15, 1.0f, 4.0f,  0, 0.0f, 0.00f, 2, 20, 75, 0xC8C4BC },
    { N_("Rebote"),         N_("Pica dos veces antes de explotar"),                4,  400, TQ_FLY_BOUNCE, TQ_FX_BLAST,    17, 1.0f, 2.2f, 26, 1.2f, 0.40f, 0, -5, 60, 0x7AB34D },
    { N_("Perforador"),     N_("Se mete en la tierra y explota abajo"),            5,  650, TQ_FLY_DRILL,  TQ_FX_BLAST,    20, 1.0f, 2.6f, 32, 2.4f, 1.10f, 2, -5, 50, 0xA0703C },
    { N_("Bombardeo"),      N_("Marcás un punto y caen cinco bombas"),             8, 1200, TQ_FLY_LOB,    TQ_FX_AIRSTRIKE, 15, 1.0f, 2.0f, 16, 1.2f, 0.40f, 1, 25, 75, 0xF5D23B },
    { N_("Francotirador"),  N_("Muy rápido, poco radio, mucho daño"),              5,  750, TQ_FLY_SHELL,  TQ_FX_BLAST,    45, 1.0f, 0.9f, 48, 0.6f, 0.25f, 3, -5, 30, 0x3A3D45 },
    { N_("Escopeta"),       N_("Cinco perdigones abiertos"),                       4,  450, TQ_FLY_SHELL,  TQ_FX_SCATTER,  21, 1.0f, 1.1f, 10, 0.6f, 0.20f, 0, -5, 40, 0xB0B6BE },
    { N_("Terremoto"),      N_("Levanta y hunde el suelo alrededor"),              7,  900, TQ_FLY_LOB,    TQ_FX_QUAKE,    15, 1.0f, 4.5f, 20, 3.5f, 0.80f, 1, 25, 75, 0x8A5A34 },
    { N_("Teletransporte"), N_("Tu tanque aparece donde cae"),                     5,  700, TQ_FLY_LOB,    TQ_FX_TELEPORT, 15, 1.0f, 0.0f,  0, 0.0f, 0.00f, 2, 20, 75, 0xC77DFF },
    { N_("Muro"),           N_("Levanta tres piedras donde cae"),                  3,  350, TQ_FLY_LOB,    TQ_FX_WALL,     14, 1.0f, 0.0f,  0, 0.0f, 0.00f, 2, 20, 75, 0xB3AB9E },
    { N_("Bomba gorda"),    N_("Lenta y enorme"),                                  8, 1500, TQ_FLY_LOB,    TQ_FX_BLAST,    13, 1.0f, 4.2f, 55, 2.8f, 1.20f, 1, 30, 75, 0x2A2D34 },
};

int tq_w_price(int w, int level)
{
    int base = TQ_WEAPON[w].price ? TQ_WEAPON[w].price : 400;
    static const int K[TQ_W_LEVELS + 1] = { 0, 35, 70, 120 };
    if (level < 1) return TQ_WEAPON[w].price;
    if (level > TQ_W_LEVELS) level = TQ_W_LEVELS;
    return base * K[level] / 100;
}

float tq_w_damage_k(int level)
{
    return 1.0f + 0.15f * level;
}

float tq_w_radius_k(int level)
{
    return 1.0f + 0.07f * level;
}

float tq_w_guide(int w, int level, int diff)
{
    /* the lobbing weapons show their whole arc; the straight ones a stretch
     * that grows with the weapon's level; hard shows less */
    float g = (TQ_WEAPON[w].fly == TQ_FLY_LOB) ? 1.0f : 0.35f + 0.2f * level;
    if (TQ_WEAPON[w].fly == TQ_FLY_BEAM) g = 1.0f;
    if (diff >= 2) g *= 0.6f;
    return g > 1 ? 1 : g;
}

/* --------------------------------------------------------------------------
 * Paint and emblems
 * -------------------------------------------------------------------------- */

const uint32_t TQ_PAINT[TQ_PAINTS] = {
    0xF58C29, 0xFAE09E, 0x478CEB, 0xF2F2FA, 0x7AB34D, 0xEDCC66,
    0xE5483B, 0x8A929C, 0x3A3D45, 0x9B6BDB, 0xF28DB5, 0x5CC8E8,
};
const char *const TQ_PAINT_NAME[TQ_PAINTS] = {
    N_("Naranja"), N_("Crema"), N_("Azul"), N_("Blanco"), N_("Verde"), N_("Arena"),
    N_("Rojo"), N_("Gris"), N_("Carbón"), N_("Violeta"), N_("Rosa"), N_("Celeste"),
};

/* Twelve emblems, drawn as 16 x 16 pixel art: a star, a heart, a bolt, a
 * paw, a moon, a crown, a flame, a flower, an anchor, a gear, a leaf and a
 * smile */
static const uint16_t EMBLEM[TQ_EMBLEMS][16] = {
    { 0x0180, 0x0180, 0x03C0, 0x03C0, 0x07E0, 0xFFFF, 0x7FFE, 0x1FF8, 0x0FF0, 0x0FF0, 0x1E78, 0x1C38, 0x381C, 0x300C, 0x6006, 0x0000 },   /* star */
    { 0x0000, 0x3C3C, 0x7E7E, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x7FFE, 0x3FFC, 0x1FF8, 0x0FF0, 0x07E0, 0x03C0, 0x0180, 0x0000, 0x0000 },   /* heart */
    { 0x007C, 0x00F8, 0x01F0, 0x03E0, 0x07C0, 0x0FF8, 0x1FF0, 0x01E0, 0x03C0, 0x0780, 0x0F00, 0x1C00, 0x3000, 0x4000, 0x0000, 0x0000 },   /* bolt */
    { 0x0000, 0x0C30, 0x1E78, 0x1E78, 0x0C30, 0x6006, 0xF00F, 0xF3CF, 0x67E6, 0x0FF0, 0x1FF8, 0x1FF8, 0x1FF8, 0x0E70, 0x0000, 0x0000 },   /* paw */
    { 0x07E0, 0x1F00, 0x3C00, 0x7800, 0x7800, 0xF000, 0xF000, 0xF000, 0xF000, 0xF000, 0x7800, 0x7800, 0x3C08, 0x1F18, 0x07F0, 0x0000 },   /* moon */
    { 0x0000, 0x0000, 0x8181, 0xC3C3, 0xE7E7, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x7FFE, 0x7FFE, 0x0000, 0x7FFE, 0x7FFE, 0x0000 },   /* crown */
    { 0x0100, 0x0180, 0x0380, 0x03C0, 0x07C8, 0x0FD8, 0x1FF8, 0x1FFC, 0x3DFC, 0x38FC, 0x387C, 0x383C, 0x1C38, 0x0E70, 0x07E0, 0x0000 },   /* flame */
    { 0x0000, 0x03C0, 0x07E0, 0x37EC, 0x7BDE, 0x7FFE, 0x3C3C, 0x1DB8, 0x1DB8, 0x3C3C, 0x7FFE, 0x7BDE, 0x37EC, 0x07E0, 0x03C0, 0x0000 },   /* flower */
    { 0x0180, 0x03C0, 0x0240, 0x03C0, 0x0180, 0x0FF0, 0x0180, 0x0180, 0x0180, 0xC183, 0xE187, 0x718E, 0x399C, 0x1FF8, 0x07E0, 0x0000 },   /* anchor */
    { 0x03C0, 0x03C0, 0x37EC, 0x3FFC, 0x1FF8, 0x7C3E, 0xF81F, 0xF00F, 0xF00F, 0xF81F, 0x7C3E, 0x1FF8, 0x3FFC, 0x37EC, 0x03C0, 0x03C0 },   /* gear */
    { 0x003F, 0x01FF, 0x07DF, 0x0FBF, 0x1F7F, 0x3EFF, 0x3DFF, 0x7BFE, 0x77FC, 0x6FF8, 0x5FE0, 0x3F80, 0x4000, 0x8000, 0x0000, 0x0000 },   /* leaf */
    { 0x07E0, 0x1FF8, 0x3FFC, 0x7FFE, 0x799E, 0xF99F, 0xFFFF, 0xFFFF, 0xFFFF, 0xEFF7, 0x77EE, 0x781E, 0x3C3C, 0x1FF8, 0x07E0, 0x0000 },   /* smile */
};

void tq_emblem(int k, uint16_t out[16])
{
    memcpy(out, EMBLEM[k % TQ_EMBLEMS], sizeof EMBLEM[0]);
}

/* --------------------------------------------------------------------------
 * The save
 * -------------------------------------------------------------------------- */

#define SAVE_VERSION 1

void tq_save_defaults(tq_save_t *s)
{
    memset(s, 0, sizeof *s);
    s->version = SAVE_VERSION;
    s->coins = 300;
    for (int t = 0; t < TQ_TANK_TYPES; t++) {
        tq_garage_t *g = &s->g[t];
        g->owned = TQ_TANK[t].price == 0;
        memset(g->load, 0xFF, sizeof g->load);
        g->load[0] = TQ_W_CANNON;
        g->load[1] = TQ_W_MORTAR;
        g->paint1 = (uint8_t)(t * 2 % TQ_PAINTS);
        g->paint2 = (uint8_t)((t * 2 + 1) % TQ_PAINTS);
        g->emblem = (uint8_t)t;
        g->emblem_paint = 6;
    }
    for (int w = 0; w < TQ_WEAPONS; w++) s->w_owned[w] = TQ_WEAPON[w].price == 0;
    s->f_biome = 0;
    s->f_size = 1;
    s->f_rivals = 1;
    s->f_diff = 1;
    s->f_players = 1;
    s->sfx = 1;
}

static void path(char *out, size_t n)
{
    snprintf(out, n, "%s/tanques.sav", aos_hal_path_data());
}

bool tq_save_load(tq_save_t *s)
{
    char p[160];
    path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    tq_save_t t;
    size_t n = fread(&t, 1, sizeof t, f);
    fclose(f);
    if (n != sizeof t || t.version != SAVE_VERSION) return false;
    *s = t;
    return true;
}

void tq_save_store(const tq_save_t *s)
{
    char p[160], q[168];
    path(p, sizeof p);
    snprintf(q, sizeof q, "%s.part", p);
    FILE *f = fopen(q, "wb");
    if (!f) return;
    bool ok = fwrite(s, 1, sizeof *s, f) == sizeof *s;
    fclose(f);
    if (ok) {
        remove(p);
        rename(q, p);
    }
}

/* --------------------------------------------------------------------------
 * The story, places and weather
 * -------------------------------------------------------------------------- */

const char *const TQ_BIOME_KEY[TQ_BIOMES] = { "pradera", "desierto", "nieve", "ruinas", "volcan" };
const char *const TQ_BIOME_NAME[TQ_BIOMES] = { N_("Pradera"), N_("Desierto"), N_("Nieve"), N_("Ruinas"), N_("Volcán") };
const char *const TQ_WEATHER_NAME[TQ_WEATHERS] = { N_("Despejado"), N_("Viento"), N_("Tormenta"), N_("Niebla") };
const float TQ_SIZE_M[TQ_SIZES] = { 28.0f, 32.0f, 36.0f, 44.0f };
const char *const TQ_SIZE_NAME[TQ_SIZES] = { N_("Chico"), N_("Mediano"), N_("Grande"), N_("Muy grande") };
const char *const TQ_DIFF_NAME[TQ_DIFFS] = { N_("Fácil"), N_("Normal"), N_("Difícil") };

const tq_mission_t TQ_MISSION[TQ_STORY_MISSIONS] = {
    /* name                         biome           size rv df weather        rival tanks                               seed  reward */
    { N_("El primer día"),          TQ_B_PRADERA,   0, 1, 0, TQ_WX_CLEAR, { TQ_T_PULGA, 0, 0 },                         11,  200 },
    { N_("La loma del medio"),      TQ_B_PRADERA,   1, 1, 0, TQ_WX_WIND,  { TQ_T_BOMBO, 0, 0 },                         23,  250 },
    { N_("Dos contra uno"),         TQ_B_PRADERA,   1, 2, 1, TQ_WX_CLEAR, { TQ_T_PULGA, TQ_T_PULGA, 0 },                37,  320 },
    { N_("Arena en los ojos"),      TQ_B_DESIERTO,  1, 1, 1, TQ_WX_WIND,  { TQ_T_OBUS, 0, 0 },                          41,  350 },
    { N_("El oasis"),               TQ_B_DESIERTO,  1, 2, 1, TQ_WX_CLEAR, { TQ_T_BOMBO, TQ_T_TOPO, 0 },                 53,  420 },
    { N_("Tormenta de arena"),      TQ_B_DESIERTO,  2, 2, 1, TQ_WX_STORM, { TQ_T_AVISPA, TQ_T_PULGA, 0 },               67,  480 },
    { N_("Bajo cero"),              TQ_B_NIEVE,     1, 2, 1, TQ_WX_FOG,   { TQ_T_MAMUT, TQ_T_PULGA, 0 },                71,  520 },
    { N_("El muñeco de nieve"),     TQ_B_NIEVE,     2, 2, 2, TQ_WX_WIND,  { TQ_T_OBUS, TQ_T_TOPO, 0 },                  83,  580 },
    { N_("Avalancha"),              TQ_B_NIEVE,     2, 3, 1, TQ_WX_STORM, { TQ_T_PULGA, TQ_T_BOMBO, TQ_T_AVISPA },      97,  650 },
    { N_("La ciudad vacía"),        TQ_B_RUINAS,    1, 2, 2, TQ_WX_FOG,   { TQ_T_AVISPA, TQ_T_MAMUT, 0 },              101,  720 },
    { N_("La plaza"),               TQ_B_RUINAS,    2, 3, 2, TQ_WX_WIND,  { TQ_T_OBUS, TQ_T_TOPO, TQ_T_AVISPA },       113,  800 },
    { N_("El volcán"),              TQ_B_VOLCAN,    2, 3, 2, TQ_WX_STORM, { TQ_T_MAMUT, TQ_T_OBUS, TQ_T_AVISPA },      127, 1000 },
};
