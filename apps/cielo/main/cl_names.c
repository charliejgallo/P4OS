/*
 * CIELO - the names the app says: the bodies, the 88 constellations, the
 * Messier objects with a name of their own, and what each kind of object
 * is. The keys are in Spanish (the catalogues translate them); the Latin
 * name of a constellation is in the pack.
 *
 * The worker draws these names, and _() must not run there (it reads the
 * catalogue of whatever app is in front, which the system frees when that
 * app closes): cl_names_prepare() translates them all once, from create().
 */
#include "cielo.h"

#include "aos_i18n.h"

#include <string.h>

static const char *const BODY[CL_NBODY] = {
    N_("Sol"), N_("Luna"), N_("Mercurio"), N_("Venus"), N_("Marte"),
    N_("Júpiter"), N_("Saturno"), N_("Urano"), N_("Neptuno"),
};
static const char *s_body[CL_NBODY];

const char *cl_body_name(int b)
{
    return b >= 0 && b < CL_NBODY ? (s_body[b] ? s_body[b] : BODY[b]) : "?";
}

static const struct {
    const char *abbr, *name;
} CONS[] = {
    { "And", N_("Andrómeda") }, { "Ant", N_("Máquina Neumática") }, { "Aps", N_("Ave del Paraíso") },
    { "Aqr", N_("Acuario") }, { "Aql", N_("Águila") }, { "Ara", N_("Altar") }, { "Ari", N_("Aries") },
    { "Aur", N_("Auriga") }, { "Boo", N_("Boyero") }, { "Cae", N_("Cincel") }, { "Cam", N_("Jirafa") },
    { "Cnc", N_("Cáncer") }, { "CVn", N_("Perros de Caza") }, { "CMa", N_("Can Mayor") },
    { "CMi", N_("Can Menor") }, { "Cap", N_("Capricornio") }, { "Car", N_("Quilla") },
    { "Cas", N_("Casiopea") }, { "Cen", N_("Centauro") }, { "Cep", N_("Cefeo") }, { "Cet", N_("Ballena") },
    { "Cha", N_("Camaleón") }, { "Cir", N_("Compás") }, { "Col", N_("Paloma") },
    { "Com", N_("Cabellera de Berenice") }, { "CrA", N_("Corona Austral") }, { "CrB", N_("Corona Boreal") },
    { "Crv", N_("Cuervo") }, { "Crt", N_("Copa") }, { "Cru", N_("Cruz del Sur") }, { "Cyg", N_("Cisne") },
    { "Del", N_("Delfín") }, { "Dor", N_("Dorado") }, { "Dra", N_("Dragón") }, { "Equ", N_("Caballito") },
    { "Eri", N_("Erídano") }, { "For", N_("Horno") }, { "Gem", N_("Géminis") }, { "Gru", N_("Grulla") },
    { "Her", N_("Hércules") }, { "Hor", N_("Reloj") }, { "Hya", N_("Hidra") }, { "Hyi", N_("Hidra Macho") },
    { "Ind", N_("Indio") }, { "Lac", N_("Lagarto") }, { "Leo", N_("Leo") }, { "LMi", N_("León Menor") },
    { "Lep", N_("Liebre") }, { "Lib", N_("Libra") }, { "Lup", N_("Lobo") }, { "Lyn", N_("Lince") },
    { "Lyr", N_("Lira") }, { "Men", N_("Mesa") }, { "Mic", N_("Microscopio") }, { "Mon", N_("Unicornio") },
    { "Mus", N_("Mosca") }, { "Nor", N_("Escuadra") }, { "Oct", N_("Octante") }, { "Oph", N_("Ofiuco") },
    { "Ori", N_("Orión") }, { "Pav", N_("Pavo") }, { "Peg", N_("Pegaso") }, { "Per", N_("Perseo") },
    { "Phe", N_("Fénix") }, { "Pic", N_("Pintor") }, { "PsA", N_("Pez Austral") }, { "Psc", N_("Piscis") },
    { "Pup", N_("Popa") }, { "Pyx", N_("Brújula") }, { "Ret", N_("Retículo") }, { "Sge", N_("Flecha") },
    { "Sgr", N_("Sagitario") }, { "Sco", N_("Escorpio") }, { "Scl", N_("Escultor") }, { "Sct", N_("Escudo") },
    { "Ser", N_("Serpiente") }, { "Sex", N_("Sextante") }, { "Tau", N_("Tauro") }, { "Tel", N_("Telescopio") },
    { "Tri", N_("Triángulo") }, { "TrA", N_("Triángulo Austral") }, { "Tuc", N_("Tucán") },
    { "UMa", N_("Osa Mayor") }, { "UMi", N_("Osa Menor") }, { "Vel", N_("Vela") }, { "Vir", N_("Virgo") },
    { "Vol", N_("Pez Volador") }, { "Vul", N_("Zorra") },
};

#define NCONS (sizeof CONS / sizeof CONS[0])
static const char *s_cons[NCONS];

const char *cl_cons_name(const char *abbr)
{
    for (unsigned i = 0; i < NCONS; i++)
        if (!strcmp(CONS[i].abbr, abbr)) return s_cons[i] ? s_cons[i] : CONS[i].name;
    return abbr;
}

/* the stars' names that are said differently in Spanish: the pack has the
 * Spanish ones (tools/names.txt), and these are translated */
static const char *const STAR_ES[] = {
    N_("Sirio"), N_("Canopo"), N_("Arturo"), N_("Proción"), N_("Aldebarán"), N_("Espiga"), N_("Pólux"),
    N_("Régulo"), N_("Cástor"), N_("Denébola"), N_("Polar"), N_("Alfard"), N_("El Persa"), N_("Alfa Centauri"),
};
#define NSTAR_ES (sizeof STAR_ES / sizeof STAR_ES[0])
static const char *s_star[NSTAR_ES];

const char *cl_star_name(const char *es)
{
    if (!es) return NULL;
    for (unsigned i = 0; i < NSTAR_ES; i++)
        if (!strcmp(STAR_ES[i], es)) return s_star[i] ? s_star[i] : es;
    return es;
}

void cl_names_prepare(void)
{
    for (int i = 0; i < CL_NBODY; i++) s_body[i] = _(BODY[i]);
    for (unsigned i = 0; i < NCONS; i++) s_cons[i] = _(CONS[i].name);
    for (unsigned i = 0; i < NSTAR_ES; i++) s_star[i] = _(STAR_ES[i]);
}

const char *cl_mess_name(int m)
{
    switch (m) {
    case 1: return _("Nebulosa del Cangrejo");
    case 6: return _("Cúmulo de la Mariposa");
    case 7: return _("Cúmulo de Ptolomeo");
    case 8: return _("Nebulosa de la Laguna");
    case 11: return _("Cúmulo del Pato Salvaje");
    case 13: return _("Gran Cúmulo de Hércules");
    case 16: return _("Nebulosa del Águila");
    case 17: return _("Nebulosa Omega");
    case 20: return _("Nebulosa Trífida");
    case 22: return _("Cúmulo de Sagitario");
    case 24: return _("Nube Estelar de Sagitario");
    case 27: return _("Nebulosa de la Haltera");
    case 31: return _("Galaxia de Andrómeda");
    case 33: return _("Galaxia del Triángulo");
    case 42: return _("Gran Nebulosa de Orión");
    case 44: return _("El Pesebre");
    case 45: return _("Las Pléyades");
    case 51: return _("Galaxia del Remolino");
    case 57: return _("Nebulosa del Anillo");
    case 64: return _("Galaxia del Ojo Negro");
    case 81: return _("Galaxia de Bode");
    case 82: return _("Galaxia del Cigarro");
    case 83: return _("Molinete Austral");
    case 97: return _("Nebulosa del Búho");
    case 101: return _("Galaxia del Molinete");
    case 104: return _("Galaxia del Sombrero");
    default: return NULL;
    }
}

const char *cl_mess_type(int t)
{
    switch (t) {
    case CL_M_OC: return _("Cúmulo abierto");
    case CL_M_GB: return _("Cúmulo globular");
    case CL_M_DI: return _("Nebulosa difusa");
    case CL_M_PL: return _("Nebulosa planetaria");
    case CL_M_SP: return _("Galaxia espiral");
    case CL_M_EL: return _("Galaxia elíptica");
    case CL_M_IR: return _("Galaxia irregular");
    case CL_M_SNR: return _("Resto de supernova");
    default: return _("Objeto de Messier");
    }
}

/* rounded published distances, light years; 0 unknown here */
float cl_mess_dist_ly(int m)
{
    static const struct {
        uint8_t m;
        float ly;
    } D[] = {
        { 1, 6500 },     { 2, 33000 },    { 3, 34000 },    { 4, 7200 },     { 5, 24500 },   { 6, 1600 },
        { 7, 980 },      { 8, 4100 },     { 11, 6200 },    { 13, 22000 },   { 15, 33000 },  { 16, 7000 },
        { 17, 5500 },    { 20, 5200 },    { 22, 10600 },   { 27, 1360 },    { 31, 2.5e6f }, { 32, 2.5e6f },
        { 33, 2.7e6f },  { 35, 2800 },    { 36, 4100 },    { 37, 4500 },    { 38, 4200 },   { 41, 2300 },
        { 42, 1340 },    { 44, 580 },     { 45, 440 },     { 47, 1600 },    { 51, 2.3e7f }, { 57, 2300 },
        { 63, 2.9e7f },  { 64, 1.7e7f },  { 67, 2700 },    { 78, 1350 },    { 81, 1.2e7f }, { 82, 1.2e7f },
        { 83, 1.5e7f },  { 87, 5.3e7f },  { 92, 26700 },   { 97, 2000 },    { 101, 2.1e7f },
        { 104, 3.1e7f }, { 106, 2.4e7f }, { 110, 2.7e6f },
    };
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; i++)
        if (D[i].m == m) return D[i].ly;
    return 0;
}
