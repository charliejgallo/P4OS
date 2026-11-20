/*
 * BASTION - the numbers (see bs_data.h)
 */
#include "bs_data.h"

#include "aos_i18n.h"

#include <string.h>

/* Each tower answers a question the aliens ask: the Gatling is cheap and
 * quick but armour shrugs it off; the cannon and the mortar punish a
 * crowd; the laser and the rail kill what is big; missiles and the Tesla
 * reach the fliers; the cold, the pulse and the well buy time; the
 * amplifier and the refinery make the rest better. */
const bs_tower_def_t BS_TOWER[TW_N] = {
    /* id        name                       desc                                                            accent    cost unl  dmg   rate  range splash air gnd spin muz hgt */
    { "gatling", N_("Ametralladora"),       N_("Barata y rápida. Floja contra las corazas."),                0xFFB020,  70,   0,   7.0f, 0.13f, 4.2f, 0.0f, 1, 1, 0, 11, 9 },
    { "cannon",  N_("Cañón"),               N_("Obuses que explotan. No llega a los que vuelan."),           0xFF4038, 120,   0,  42.0f, 1.15f, 4.6f, 1.3f, 0, 1, 0, 12, 10 },
    { "laser",   N_("Láser"),               N_("Un rayo que quema más cuanto más dura sobre el mismo."),      0xFF3BD4, 170,  45,  34.0f, 0.0f,  4.8f, 0.0f, 1, 1, 0, 9, 13 },
    { "missile", N_("Misiles"),             N_("Persiguen de lejos. Lo mejor contra los que vuelan."),        0x9BE830, 180,   0,  60.0f, 1.9f,  6.6f, 1.0f, 1, 1, 0, 6, 14 },
    { "tesla",   N_("Tesla"),               N_("Un rayo que salta de uno a otro."),                           0x40B8FF, 200,  60,  30.0f, 0.85f, 3.9f, 0.0f, 1, 1, 1, 0, 22 },
    { "cryo",    N_("Criogénica"),          N_("Frena a todos los que tiene cerca."),                         0xA8F4FF, 130,   0,   5.0f, 0.0f,  3.5f, 0.0f, 1, 1, 1, 0, 12 },
    { "flame",   N_("Lanzallamas"),         N_("Un chorro corto que deja ardiendo a la fila entera."),        0xFF7A1A, 150,  55,  46.0f, 0.0f,  3.1f, 0.0f, 0, 1, 0, 10, 9 },
    { "rail",    N_("Cañón de riel"),       N_("Atraviesa a todos los de la línea. Lento y carísimo."),       0xA070FF, 280,  90, 190.0f, 2.9f,  9.0f, 0.0f, 0, 1, 0, 16, 11 },
    { "mortar",  N_("Mortero de plasma"),   N_("Explosiones enormes muy lejos. Ciego de cerca."),             0x2EE6C0, 240,  80, 100.0f, 3.3f, 10.0f, 2.2f, 0, 1, 0, 6, 16 },
    { "drone",   N_("Hangar de drones"),    N_("Drones que salen a buscar a los que se escapan."),            0xFF6FA8, 230,  95,  15.0f, 0.0f,  5.6f, 0.0f, 1, 1, 1, 0, 14 },
    { "emp",     N_("Pulso EMP"),           N_("Paraliza a los de alrededor y les apaga los escudos."),       0x5A6CFF, 190,  70,  22.0f, 4.2f,  3.6f, 0.0f, 1, 1, 1, 0, 12 },
    { "gravity", N_("Pozo de gravedad"),    N_("Los frena y cada tanto los tira para atrás."),                0xC040FF, 290, 110,  12.0f, 6.0f,  4.0f, 0.0f, 1, 1, 1, 0, 14 },
    { "radar",   N_("Amplificador"),        N_("Las torres vecinas pegan más y ven a los espectros."),        0x38FF9C, 160,  65,   0.0f, 0.0f,  3.7f, 0.0f, 1, 1, 1, 0, 16 },
    { "bank",    N_("Refinería"),           N_("Da créditos al final de cada oleada."),                       0xFFD83A, 200,  75,  45.0f, 0.0f,  0.0f, 0.0f, 0, 0, 1, 0, 10 },
};

/* A boss's reward is what it pays when it falls; it has already paid
 * BS_BOSS_PARTS x BS_BOSS_PART (300) on the way down (bs_game.c, hurt). */
const bs_alien_def_t BS_ALIEN[AL_N] = {
    /* id         name               hp     speed armor rew lives fly boss xp radius alt  shield  color   */
    { "grub",     N_("Larva"),        42.0f, 1.20f, 0,   5,  1,   0, 0,  1, 0.45f, 0.0f,   0.0f, 0x6CFF4A },
    { "runner",   N_("Corredor"),     28.0f, 2.40f, 0,   6,  1,   0, 0,  1, 0.40f, 0.0f,   0.0f, 0xFFE040 },
    { "swarm",    N_("Ácaro"),        11.0f, 1.90f, 0,   2,  1,   0, 0,  1, 0.28f, 0.0f,   0.0f, 0xFF5040 },
    { "shell",    N_("Coraza"),      160.0f, 0.90f, 7,  14,  1,   0, 0,  3, 0.55f, 0.0f,   0.0f, 0x70A0FF },
    { "wasp",     N_("Avispa"),       48.0f, 2.00f, 0,   9,  1,   1, 0,  2, 0.45f, 1.6f,   0.0f, 0xFF9030 },
    { "split",    N_("Divisor"),     115.0f, 1.10f, 0,  10,  1,   0, 0,  2, 0.50f, 0.0f,   0.0f, 0xC060FF },
    { "shield",   N_("Escudero"),     95.0f, 1.20f, 0,  14,  1,   0, 0,  3, 0.50f, 0.0f, 130.0f, 0x40E8FF },
    { "healer",   N_("Sanador"),     125.0f, 1.10f, 0,  16,  1,   0, 0,  3, 0.50f, 0.5f,   0.0f, 0x80FFC0 },
    { "brute",    N_("Bruto"),       950.0f, 0.60f, 3,  45,  3,   0, 0,  8, 0.95f, 0.0f,   0.0f, 0xD03030 },
    { "phase",    N_("Espectro"),    105.0f, 1.50f, 0,  14,  1,   0, 0,  3, 0.45f, 0.0f,   0.0f, 0xC8B0FF },
    { "jammer",   N_("Saboteador"),  170.0f, 1.00f, 0,  18,  1,   0, 0,  4, 0.50f, 0.6f,   0.0f, 0xFF40C0 },
    { "carrier",  N_("Portador"),    760.0f, 0.80f, 2,  50,  3,   1, 0, 10, 1.00f, 2.2f,   0.0f, 0x30D0B0 },
    { "queen",    N_("Reina"),      4600.0f, 0.45f, 4, 100, 10,   0, 1, 60, 1.60f, 0.0f,   0.0f, 0xB0FF40 },
    { "titan",    N_("Titán"),      7200.0f, 0.40f, 10, 100, 10,  0, 1, 60, 1.60f, 0.0f,   0.0f, 0xFF5040 },
    { "overmind", N_("Mente"),      5200.0f, 0.50f, 0, 100, 10,   0, 1, 60, 1.50f, 1.0f, 1500.0f, 0xB070FF },
    { "mini",     N_("Divisor"),      32.0f, 1.70f, 0,   3,  1,   0, 0,  1, 0.30f, 0.0f,   0.0f, 0xC060FF },
};

const bs_tech_def_t BS_TECH[TECH_N] = {
    { N_("Blindaje"),      N_("+3 de vida del fuerte por nivel"),               { 20, 35, 55, 80, 120 } },
    { N_("Financiación"),  N_("+10 % de créditos al empezar"),                  { 15, 30, 50, 75, 110 } },
    { N_("Recompensas"),   N_("+6 % de créditos por cada alien"),               { 20, 35, 55, 85, 125 } },
    { N_("Munición"),      N_("+5 % de daño de todas las torres"),              { 25, 45, 70, 100, 150 } },
    { N_("Óptica"),        N_("+3 % de alcance de todas las torres"),           { 20, 40, 60, 90, 130 } },
    { N_("Reciclaje"),     N_("+6 % al vender una torre"),                      { 10, 20, 35, 55, 80 } },
    { N_("Interés"),       N_("+2 % de lo ahorrado al terminar cada oleada"),   { 25, 45, 70, 100, 140 } },
    { N_("Puntería"),      N_("+4 % de golpes críticos, que pegan el doble"),   { 25, 45, 70, 105, 150 } },
};

const bs_power_def_t BS_POWER[PW_N] = {
    { N_("Ataque orbital"), N_("Un rayo del cielo donde toques."),          { 50, 70, 110 }, 45.0f, 0xFF7030 },
    { N_("Criobomba"),      N_("Congela a todos unos segundos."),           { 70, 90, 130 }, 60.0f, 0x80E8FF },
    { N_("Reparación"),     N_("Le devuelve vida al fuerte."),              { 40, 60, 100 }, 75.0f, 0x60FF90 },
};

const int16_t BS_MASTERY_COST[BS_MASTERY_MAX] = { 15, 25, 40, 60, 90 };

int bs_alien_by_id(const char *id)
{
    for (int i = 0; i < AL_N; i++) {
        if (!strcmp(BS_ALIEN[i].id, id)) return i;
    }
    return -1;
}

float bs_tier_dmg(int type, int tier)
{
    static const float K[BS_TIERS] = { 1.0f, 1.75f, 3.0f };
    return BS_TOWER[type].dmg * K[tier];
}

float bs_tier_range(int type, int tier)
{
    if (BS_TOWER[type].range <= 0) return 0;
    return BS_TOWER[type].range + 0.4f * (float)tier;
}

int bs_tier_cost(int type, int tier)
{
    static const int K[BS_TIERS] = { 100, 80, 130 };
    return (BS_TOWER[type].cost * K[tier] + 50) / 100;
}

int bs_rank_xp(int rank)
{
    /* rank 1 at once; each one asks a little more than the last */
    if (rank <= 1) return 0;
    int n = rank - 1;
    return 60 * n + 22 * n * n;
}

int bs_rank_of(int xp)
{
    int r = 1;
    while (r < BS_RANK_MAX && xp >= bs_rank_xp(r + 1)) r++;
    return r;
}
