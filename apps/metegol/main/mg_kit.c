/*
 * METEGOL - the teams and their kits (see mg_kit.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mg_kit.h"

#include "mg_pak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const MG_DESIGN_WORD[MG_D_N] = { "lisa", "rayas", "bastones", "franja", "banda", "v", "mitades", "cuadros",
                                             "vivos", "aros" };

/* What the app writes the first time. Colours as the clubs and the
 * national teams wear them, without crests; the sixteen national teams'
 * flags in a few strokes. */
static const char DEFAULT_TEAMS[] =
    "# Metegol: los equipos. Lo lee la app al abrir un partido; se edita acá\n"
    "# o desde la página del portal. Un bloque por equipo:\n"
    "#   nombre, corto (tres letras del marcador), liga (selecciones o clubes),\n"
    "#   diseño: lisa, rayas, bastones, franja, banda, v, mitades, cuadros, vivos o aros,\n"
    "#   color1 (el de la camiseta), color2 (el del diseño), mangas, cuello, pantalon, medias,\n"
    "#   estadio (el de local, si tiene) y bandera (las selecciones).\n"
    "# Una bandera son trazos separados por ';': h:c1,c2,... (franjas horizontales;\n"
    "# c*2 la hace el doble de ancha), v:... (verticales), disc:color:x,y,r,\n"
    "# cross:color:ancho, canton:color:ancho,alto, rombo:color, cuadros:color:x,y,lado,\n"
    "# star:color:x,y,r. x, y y los tamaños van de 0 a 1.\n"
    "\n"
    "[argentina]\nnombre = Argentina\ncorto = ARG\nliga = selecciones\ndiseño = bastones\ncolor1 = 75AADB\ncolor2 = FFFFFF\n"
    "mangas = 75AADB\ncuello = 1C2F5E\npantalon = 1C1C24\nmedias = FFFFFF\n"
    "bandera = h:75AADB,FFFFFF,75AADB;disc:F6B40E:0.5,0.5,0.11\n\n"
    "[brasil]\nnombre = Brasil\ncorto = BRA\nliga = selecciones\ndiseño = vivos\ncolor1 = FFDC02\ncolor2 = 009C3B\n"
    "mangas = FFDC02\ncuello = 009C3B\npantalon = 1E3C96\nmedias = FFFFFF\n"
    "bandera = h:009C3B;rombo:FFDF00;disc:002776:0.5,0.5,0.2\n\n"
    "[espana]\nnombre = España\ncorto = ESP\nliga = selecciones\ndiseño = vivos\ncolor1 = C60B1E\ncolor2 = FFC400\n"
    "mangas = C60B1E\ncuello = FFC400\npantalon = 14215A\nmedias = 14215A\n"
    "bandera = h:AA151B,F1BF00*2,AA151B\n\n"
    "[alemania]\nnombre = Alemania\ncorto = ALE\nliga = selecciones\ndiseño = vivos\ncolor1 = F4F4F4\ncolor2 = 1A1A1A\n"
    "mangas = F4F4F4\ncuello = 1A1A1A\npantalon = 1A1A1A\nmedias = F4F4F4\n"
    "bandera = h:1A1A1A,DD0000,FFCE00\n\n"
    "[italia]\nnombre = Italia\ncorto = ITA\nliga = selecciones\ndiseño = vivos\ncolor1 = 1F5FB4\ncolor2 = FFFFFF\n"
    "mangas = 1F5FB4\ncuello = FFFFFF\npantalon = FFFFFF\nmedias = 1F5FB4\n"
    "bandera = v:009246,FFFFFF,CE2B37\n\n"
    "[mexico]\nnombre = México\ncorto = MEX\nliga = selecciones\ndiseño = vivos\ncolor1 = 006847\ncolor2 = FFFFFF\n"
    "mangas = 006847\ncuello = CE1126\npantalon = FFFFFF\nmedias = CE1126\n"
    "bandera = v:006847,FFFFFF,CE1126;disc:8C6A3B:0.5,0.5,0.1\n\n"
    "[eeuu]\nnombre = Estados Unidos\ncorto = USA\nliga = selecciones\ndiseño = vivos\ncolor1 = F4F4F4\ncolor2 = 0A3161\n"
    "mangas = F4F4F4\ncuello = 0A3161\npantalon = 0A3161\nmedias = F4F4F4\n"
    "bandera = h:B22234,FFFFFF,B22234,FFFFFF,B22234,FFFFFF,B22234;canton:3C3B6E:0.42,0.58\n\n"
    "[japon]\nnombre = Japón\ncorto = JPN\nliga = selecciones\ndiseño = vivos\ncolor1 = 1B3C8C\ncolor2 = FFFFFF\n"
    "mangas = 1B3C8C\ncuello = D0102E\npantalon = FFFFFF\nmedias = 1B3C8C\n"
    "bandera = h:FFFFFF;disc:BC002D:0.5,0.5,0.3\n\n"
    "[uruguay]\nnombre = Uruguay\ncorto = URU\nliga = selecciones\ndiseño = lisa\ncolor1 = 7BB2DD\ncolor2 = 1A1A1A\n"
    "mangas = 7BB2DD\ncuello = 1A1A1A\npantalon = 1A1A1A\nmedias = 1A1A1A\n"
    "bandera = h:FFFFFF,0038A8,FFFFFF,0038A8,FFFFFF,0038A8,FFFFFF,0038A8,FFFFFF;canton:FFFFFF:0.38,0.56;"
    "disc:FCD116:0.19,0.28,0.13\n\n"
    "[francia]\nnombre = Francia\ncorto = FRA\nliga = selecciones\ndiseño = vivos\ncolor1 = 1C2C5B\ncolor2 = FFFFFF\n"
    "mangas = 1C2C5B\ncuello = E1232C\npantalon = FFFFFF\nmedias = E1232C\n"
    "bandera = v:0055A4,FFFFFF,EF4135\n\n"
    "[inglaterra]\nnombre = Inglaterra\ncorto = ING\nliga = selecciones\ndiseño = vivos\ncolor1 = F4F4F4\ncolor2 = 0A1F44\n"
    "mangas = F4F4F4\ncuello = 0A1F44\npantalon = 0A1F44\nmedias = F4F4F4\n"
    "bandera = h:FFFFFF;cross:CE1124:0.2\n\n"
    "[paisesbajos]\nnombre = Países Bajos\ncorto = HOL\nliga = selecciones\ndiseño = vivos\ncolor1 = FF6A00\ncolor2 = 1A1A1A\n"
    "mangas = FF6A00\ncuello = 1A1A1A\npantalon = FF6A00\nmedias = FF6A00\n"
    "bandera = h:AE1C28,FFFFFF,21468B\n\n"
    "[portugal]\nnombre = Portugal\ncorto = POR\nliga = selecciones\ndiseño = vivos\ncolor1 = C8102E\ncolor2 = 0B6E2E\n"
    "mangas = C8102E\ncuello = 0B6E2E\npantalon = 0B6E2E\nmedias = C8102E\n"
    "bandera = v:006600*2,FF0000*3;disc:FFE000:0.4,0.5,0.15\n\n"
    "[croacia]\nnombre = Croacia\ncorto = CRO\nliga = selecciones\ndiseño = cuadros\ncolor1 = FFFFFF\ncolor2 = E4002B\n"
    "mangas = FFFFFF\ncuello = 1A3E8C\npantalon = FFFFFF\nmedias = 1A3E8C\n"
    "bandera = h:FF0000,FFFFFF,171796;cuadros:FF0000:0.5,0.5,0.26\n\n"
    "[colombia]\nnombre = Colombia\ncorto = COL\nliga = selecciones\ndiseño = vivos\ncolor1 = FCD116\ncolor2 = 003893\n"
    "mangas = FCD116\ncuello = 003893\npantalon = 003893\nmedias = CE1126\n"
    "bandera = h:FCD116*2,003893,CE1126\n\n"
    "[marruecos]\nnombre = Marruecos\ncorto = MAR\nliga = selecciones\ndiseño = vivos\ncolor1 = C1272D\ncolor2 = 006233\n"
    "mangas = C1272D\ncuello = 006233\npantalon = 006233\nmedias = C1272D\n"
    "bandera = h:C1272D;star:006233:0.5,0.5,0.24\n\n"
    "[boca]\nnombre = Boca Juniors\ncorto = BOC\nliga = clubes\ndiseño = franja\ncolor1 = 0B3D91\ncolor2 = FCD116\n"
    "mangas = 0B3D91\ncuello = FCD116\npantalon = 0B3D91\nmedias = 0B3D91\nestadio = laboca\n\n"
    "[river]\nnombre = River Plate\ncorto = RIV\nliga = clubes\ndiseño = banda\ncolor1 = F6F6F6\ncolor2 = E30613\n"
    "mangas = F6F6F6\ncuello = E30613\npantalon = 1A1A1A\nmedias = F6F6F6\nestadio = nunez\n\n"
    "[independiente]\nnombre = Independiente\ncorto = IND\nliga = clubes\ndiseño = lisa\ncolor1 = D7141A\ncolor2 = D7141A\n"
    "mangas = D7141A\ncuello = D7141A\npantalon = 14215A\nmedias = D7141A\nestadio = avella_rojo\n\n"
    "[racing]\nnombre = Racing Club\ncorto = RAC\nliga = clubes\ndiseño = bastones\ncolor1 = 7FBCE8\ncolor2 = FFFFFF\n"
    "mangas = 7FBCE8\ncuello = 1A1A1A\npantalon = 1A1A1A\nmedias = 1A1A1A\nestadio = avella_celeste\n\n"
    "[sanlorenzo]\nnombre = San Lorenzo\ncorto = SLO\nliga = clubes\ndiseño = bastones\ncolor1 = 1B2D7C\ncolor2 = C8102E\n"
    "mangas = 1B2D7C\ncuello = C8102E\npantalon = 1B2D7C\nmedias = 1B2D7C\n\n"
    "[velez]\nnombre = Vélez Sarsfield\ncorto = VEL\nliga = clubes\ndiseño = v\ncolor1 = F6F6F6\ncolor2 = 1550A8\n"
    "mangas = F6F6F6\ncuello = 1550A8\npantalon = 1550A8\nmedias = F6F6F6\n\n"
    "[huracan]\nnombre = Huracán\ncorto = HUR\nliga = clubes\ndiseño = vivos\ncolor1 = F6F6F6\ncolor2 = E2001A\n"
    "mangas = F6F6F6\ncuello = E2001A\npantalon = F6F6F6\nmedias = F6F6F6\n\n"
    "[central]\nnombre = Rosario Central\ncorto = CEN\nliga = clubes\ndiseño = bastones\ncolor1 = 0A36A0\ncolor2 = FFD100\n"
    "mangas = 0A36A0\ncuello = FFD100\npantalon = 0A36A0\nmedias = 0A36A0\n\n"
    "[newells]\nnombre = Newell's Old Boys\ncorto = NOB\nliga = clubes\ndiseño = mitades\ncolor1 = E1001A\ncolor2 = 1A1A1A\n"
    "mangas = E1001A\ncuello = 1A1A1A\npantalon = 1A1A1A\nmedias = 1A1A1A\n\n"
    "[estudiantes]\nnombre = Estudiantes\ncorto = EST\nliga = clubes\ndiseño = rayas\ncolor1 = E30613\ncolor2 = FFFFFF\n"
    "mangas = E30613\ncuello = FFFFFF\npantalon = 1A1A1A\nmedias = E30613\n\n"
    "[argentinos]\nnombre = Argentinos Juniors\ncorto = AAJ\nliga = clubes\ndiseño = vivos\ncolor1 = E2001A\ncolor2 = FFFFFF\n"
    "mangas = E2001A\ncuello = FFFFFF\npantalon = FFFFFF\nmedias = E2001A\n\n"
    "[talleres]\nnombre = Talleres\ncorto = TAL\nliga = clubes\ndiseño = bastones\ncolor1 = 0F2350\ncolor2 = FFFFFF\n"
    "mangas = 0F2350\ncuello = FFFFFF\npantalon = 0F2350\nmedias = 0F2350\n";

/* the next line of a text, cut in place (strtok is not in the apps' table) */
static char *next_line(char **p)
{
    char *s = *p;
    if (!s || !*s) return NULL;
    char *e = strchr(s, '\n');
    if (e) {
        *e = 0;
        *p = e + 1;
    } else {
        *p = s + strlen(s);
    }
    return s;
}

/* ---- the file -------------------------------------------------------------------------------------- */

static uint32_t file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint32_t)st.st_mtime ? (uint32_t)st.st_mtime : 1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static uint32_t hexcol(const char *s, uint32_t def)
{
    while (*s == '#' || *s == ' ') s++;
    char *e;
    unsigned long v = strtoul(s, &e, 16);
    return e != s ? (uint32_t)(v & 0xFFFFFF) : def;
}

static void team_defaults(mg_team_t *t)
{
    memset(t, 0, sizeof *t);
    t->c1 = 0xF4F4F4;
    t->c2 = 0x1A1A1A;
    t->sleeves = t->c1;
    t->collar = t->c2;
    t->shorts = 0x1A1A1A;
    t->socks = t->c1;
}

static void parse(mg_teams_t *ts, char *txt)
{
    ts->n = 0;
    mg_team_t *t = NULL;
    bool sleeves_set = false;
    char *cur = txt;
    for (char *line; (line = next_line(&cur));) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            if (t && !sleeves_set) t->sleeves = t->c1;
            if (ts->n >= MG_TEAMS_MAX) {
                t = NULL;
                continue;
            }
            t = &ts->t[ts->n++];
            team_defaults(t);
            sleeves_set = false;
            char *e = strchr(line, ']');
            if (e) *e = 0;
            copy(t->id, sizeof t->id, line + 1);
            copy(t->name, sizeof t->name, line + 1);
            continue;
        }
        if (!t) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "nombre")) copy(t->name, sizeof t->name, v);
        else if (!strcmp(k, "corto")) copy(t->abbr, sizeof t->abbr, v);
        else if (!strcmp(k, "liga")) t->league = (v[0] == 'c' || v[0] == 'C') ? MG_LEAGUE_CLUB : MG_LEAGUE_NAT;
        else if (!strcmp(k, "diseño") || !strcmp(k, "diseno")) {
            for (int i = 0; i < MG_D_N; i++) {
                if (!strcmp(v, MG_DESIGN_WORD[i])) t->design = (uint8_t)i;
            }
        } else if (!strcmp(k, "color1")) t->c1 = hexcol(v, t->c1);
        else if (!strcmp(k, "color2")) t->c2 = hexcol(v, t->c2);
        else if (!strcmp(k, "mangas")) {
            t->sleeves = hexcol(v, t->sleeves);
            sleeves_set = true;
        } else if (!strcmp(k, "cuello")) t->collar = hexcol(v, t->collar);
        else if (!strcmp(k, "pantalon") || !strcmp(k, "pantalón")) t->shorts = hexcol(v, t->shorts);
        else if (!strcmp(k, "medias")) t->socks = hexcol(v, t->socks);
        else if (!strcmp(k, "estadio")) copy(t->stadium, sizeof t->stadium, v);
        else if (!strcmp(k, "bandera")) copy(t->flag, sizeof t->flag, v);
    }
    if (t && !sleeves_set) t->sleeves = t->c1;
    for (int i = 0; i < ts->n; i++) {
        mg_team_t *u = &ts->t[i];
        if (!u->abbr[0]) {
            for (int k = 0; k < 3 && u->name[k]; k++) u->abbr[k] = (char)(u->name[k] >= 'a' && u->name[k] <= 'z' ? u->name[k] - 32 : u->name[k]);
        }
    }
}

void mg_teams_load(mg_teams_t *ts, const char *dir)
{
    char path[200];
    snprintf(path, sizeof path, "%.150s/metegol", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%.150s/metegol/equipos.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        FILE *w = fopen(path, "wb");
        if (w) {
            fwrite(DEFAULT_TEAMS, 1, sizeof DEFAULT_TEAMS - 1, w);
            fclose(w);
        }
        f = fopen(path, "rb");
    }
    char *txt = NULL;
    long n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 256 * 1024) {
            txt = (char *)mg_malloc((size_t)n + 1);
            if (txt && fread(txt, 1, (size_t)n, f) != (size_t)n) n = 0;
        }
        fclose(f);
    }
    if (txt) {
        txt[n] = 0;
        parse(ts, txt);
        mg_free(txt);
    }
    /* a file with too few of either league: the defaults, so a match can be had */
    int nat = mg_teams_of(ts, MG_LEAGUE_NAT, NULL, 0), club = mg_teams_of(ts, MG_LEAGUE_CLUB, NULL, 0);
    if (nat < 2 || club < 2) {
        char *d = (char *)mg_malloc(sizeof DEFAULT_TEAMS);
        if (d) {
            memcpy(d, DEFAULT_TEAMS, sizeof DEFAULT_TEAMS);
            parse(ts, d);
            mg_free(d);
        }
    }
    ts->mtime = file_mtime(path);
}

bool mg_teams_changed(const mg_teams_t *ts, const char *dir)
{
    char path[200];
    snprintf(path, sizeof path, "%.150s/metegol/equipos.txt", dir);
    return file_mtime(path) != ts->mtime;
}

int mg_teams_of(const mg_teams_t *ts, int league, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ts->n; i++) {
        if (ts->t[i].league != league) continue;
        if (out && n < max) out[n] = i;
        n++;
    }
    return out && n > max ? max : n;
}

int mg_teams_find(const mg_teams_t *ts, const char *id)
{
    for (int i = 0; i < ts->n; i++) {
        if (!strcmp(ts->t[i].id, id)) return i;
    }
    return -1;
}

/* ---- the shirt ---------------------------------------------------------------------------------- */

uint32_t mg_kit_shirt(const mg_team_t *t, float f, float l, float z)
{
    const float PI = 3.14159265f;
    switch (t->design) {
    case MG_D_STRIPES:
    case MG_D_BARS: {
        /* stripes go round the body: so many a turn, one centred on the chest */
        float n = t->design == MG_D_BARS ? 10.0f : 16.0f;
        float a = atan2f(l, f) / (2.0f * PI) + 0.5f;    /* 0..1 round the body, 0.5 the front */
        int k = (int)floorf((a - 0.5f) * n + 0.5f + 1000.0f);
        return (k & 1) ? t->c2 : t->c1;
    }
    case MG_D_BAND:
        return z > 0.40f && z < 0.68f ? t->c2 : t->c1;
    case MG_D_HOOPS: {
        int k = (int)floorf(z * 7.0f);
        return (k & 1) ? t->c2 : t->c1;
    }
    case MG_D_SASH: {
        /* the right shoulder to the left hip in front, and on round the back:
         * a plane slanted across the body and along it, so that from the side
         * it also runs from high to low */
        float d = z - (0.52f - 0.30f * l + 0.16f * f);
        return fabsf(d) < 0.15f ? t->c2 : t->c1;
    }
    case MG_D_V: {
        /* from the shoulders down to the middle of the chest, in front */
        if (f < -0.15f) return t->c1;
        float d = z - (0.55f + 0.42f * fabsf(l)) - 0.12f * (1.0f - f);
        return fabsf(d) < 0.11f ? t->c2 : t->c1;
    }
    case MG_D_HALVES:
        return l > 0 ? t->c2 : t->c1;
    case MG_D_CHECKS: {
        float a = atan2f(l, f) / (2.0f * PI) + 0.5f;
        int i = (int)floorf(a * 14.0f + 100.0f), j = (int)floorf(z * 6.0f);
        return ((i + j) & 1) ? t->c2 : t->c1;
    }
    case MG_D_TRIM:
        /* a line down each side, below the arm */
        return fabsf(f) < 0.16f && fabsf(l) > 0.7f && z < 0.82f ? t->c2 : t->c1;
    default:
        return t->c1;
    }
}

static inline uint16_t shade565(uint32_t rgb, int s)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int k = s > 160 ? 160 : s;
    int hi = s > 160 ? (s - 160) * 2 : 0;
    r = r * k / 160 + hi;
    g = g * k / 160 + hi;
    b = b * k / 160 + hi;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint8_t *mg_kit_paint(const uint8_t *m, size_t len, const mg_team_t *t, bool mirror, size_t *out_len)
{
    if (!m || len < 8 || memcmp(m, "MGFG", 4)) return NULL;
    int n = m[4] | (m[5] << 8), cols = m[6] | (m[7] << 8);
    if (n <= 0 || 8 + (size_t)n * 12 > len) return NULL;
    mg_rgba_frame_t *fr = (mg_rgba_frame_t *)mg_calloc(sizeof(mg_rgba_frame_t) * (size_t)n);
    if (!fr) return NULL;
    uint8_t *res = NULL;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        const uint8_t *e = m + 8 + 12 * (size_t)i;
        int ox = (int16_t)(e[0] | (e[1] << 8)), oy = (int16_t)(e[2] | (e[3] << 8));
        int w = e[4] | (e[5] << 8), h = e[6] | (e[7] << 8);
        uint32_t off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        if ((size_t)off + (size_t)w * h * 6 > len || w <= 0 || h <= 0) {
            fr[i].w = fr[i].h = 0;
            continue;
        }
        uint16_t *px = (uint16_t *)mg_malloc((size_t)w * h * 2);
        uint8_t *al = (uint8_t *)mg_malloc((size_t)w * h);
        if (!px || !al) {
            mg_free(px);
            mg_free(al);
            ok = false;
            break;
        }
        const uint8_t *src = m + off;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint8_t *p = src + ((size_t)y * w + x) * 6;
                int dx = mirror ? w - 1 - x : x;
                size_t o = (size_t)y * w + dx;
                al[o] = p[0];
                if (p[0] < 8) {
                    px[o] = 0;
                    continue;
                }
                float ff = ((float)p[3] - 128.0f) / 127.0f, ll = ((float)p[4] - 128.0f) / 127.0f;
                float zz = (float)p[5] / 255.0f;
                /* the mirror is the same figure facing the other way: its left is our right */
                if (mirror) ll = -ll;
                uint32_t c;
                switch (p[2]) {
                case MG_P_SKIN:   c = 0xE0A37A; break;
                case MG_P_HAIR:   c = 0x2A1C14; break;
                case MG_P_SHIRT:  c = mg_kit_shirt(t, ff, ll, zz); break;
                case MG_P_SLEEVE:
                    /* the cuff in the trim's colour for a trimmed shirt */
                    c = t->design == MG_D_TRIM && zz < 0.25f ? t->c2 : t->sleeves;
                    if (t->design == MG_D_HALVES) c = ll > 0 ? t->c2 : t->c1;
                    break;
                case MG_P_COLLAR: c = t->collar; break;
                case MG_P_SHORTS: c = t->shorts; break;
                case MG_P_SOCKS:  c = zz > 0.7f && t->design != MG_D_PLAIN ? t->c2 : t->socks; break;
                case MG_P_BOOTS:  c = 0x1A1A1A; break;
                default:          c = 0xB8BCC4; break;
                }
                px[o] = shade565(c, p[1]);
            }
        }
        fr[i].px = px;
        fr[i].al = al;
        fr[i].w = w;
        fr[i].h = h;
        /* mg_sheet_encode takes where the anchor is inside the frame; the
         * mirror keeps the anchor and turns the columns round it */
        fr[i].ox = mirror ? w - 1 + ox : -ox;
        fr[i].oy = -oy;
    }
    if (ok) res = mg_sheet_encode(fr, n, cols, out_len);
    for (int i = 0; i < n; i++) {
        mg_free((void *)fr[i].px);
        mg_free((void *)fr[i].al);
    }
    mg_free(fr);
    return res;
}

/* ---- badges and flags ----------------------------------------------------------------------------- */

static uint16_t c565(uint32_t rgb)
{
    return mg_rgb(rgb);
}

static void fill_rect(uint16_t *px, int w, int h, float x0, float y0, float x1, float y1, uint16_t c)
{
    int a = (int)(x0 * w + 0.5f), b = (int)(x1 * w + 0.5f), cc = (int)(y0 * h + 0.5f), d = (int)(y1 * h + 0.5f);
    if (a < 0) a = 0;
    if (cc < 0) cc = 0;
    if (b > w) b = w;
    if (d > h) d = h;
    for (int y = cc; y < d; y++)
        for (int x = a; x < b; x++) px[y * w + x] = c;
}

/* one stroke of a flag: "h:c1,c2*2,..." and the rest (mg_kit.h) */
static void stroke(char *s, uint16_t *px, int w, int h)
{
    char *colon = strchr(s, ':');
    if (!colon) return;
    *colon = 0;
    char *k = s, *v = colon + 1;
    if (!strcmp(k, "h") || !strcmp(k, "v")) {
        uint32_t col[16];
        int wt[16], n = 0, tot = 0;
        for (char *p = v; *p && n < 16;) {
            char *e = strchr(p, ',');
            if (e) *e = 0;
            col[n] = hexcol(p, 0);
            char *st = strchr(p, '*');
            wt[n] = st ? atoi(st + 1) : 1;
            if (wt[n] < 1) wt[n] = 1;
            tot += wt[n];
            n++;
            if (!e) break;
            p = e + 1;
        }
        float at = 0;
        for (int i = 0; i < n; i++) {
            float b = at + (float)wt[i] / (float)tot;
            if (k[0] == 'h') fill_rect(px, w, h, 0, at, 1, b, c565(col[i]));
            else fill_rect(px, w, h, at, 0, b, 1, c565(col[i]));
            at = b;
        }
        return;
    }
    char *colon2 = strchr(v, ':');
    if (colon2) *colon2 = 0;
    uint16_t c = c565(hexcol(v, 0));
    float a[4] = { 0.5f, 0.5f, 0.2f, 0.2f };
    if (colon2) {
        char *p = colon2 + 1;
        for (int i = 0; i < 4 && p; i++) {
            a[i] = strtof(p, NULL);
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    if (!strcmp(k, "disc") || !strcmp(k, "star")) {
        float cx = a[0] * w, cy = a[1] * h, r = a[2] * h;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                bool in;
                if (k[0] == 'd') {
                    in = dx * dx + dy * dy <= r * r;
                } else {
                    /* a five-pointed outline, as a ring of a star's radius */
                    float an = atan2f(dy, dx) + 1.5708f, d = sqrtf(dx * dx + dy * dy);
                    float m = fmodf(an + 62.832f, 1.2566f) / 1.2566f;
                    float edge = r * (0.45f + 0.55f * fabsf(1.0f - 2.0f * m));
                    in = d <= edge && d >= edge - (float)h * 0.07f;
                }
                if (in) px[y * w + x] = c;
            }
        }
    } else if (!strcmp(k, "cross")) {
        float t = a[0] > 0 && a[0] < 1 ? a[0] : 0.2f;
        fill_rect(px, w, h, 0.5f - t * (float)h / (float)w * 0.5f, 0, 0.5f + t * (float)h / (float)w * 0.5f, 1, c);
        fill_rect(px, w, h, 0, 0.5f - t * 0.5f, 1, 0.5f + t * 0.5f, c);
    } else if (!strcmp(k, "canton")) {
        fill_rect(px, w, h, 0, 0, a[0], a[1], c);
    } else if (!strcmp(k, "rombo")) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float dx = fabsf(((float)x + 0.5f) / w - 0.5f) / 0.42f, dy = fabsf(((float)y + 0.5f) / h - 0.5f) / 0.38f;
                if (dx + dy <= 1.0f) px[y * w + x] = c;
            }
        }
    } else if (!strcmp(k, "cuadros")) {
        float s = a[2] > 0 ? a[2] : 0.26f;
        int x0 = (int)((a[0] - s * 0.5f * h / w) * w), y0 = (int)((a[1] - s * 0.5f) * h), side = (int)(s * h);
        if (side < 4) side = 4;
        int cell = side / 4 > 0 ? side / 4 : 1;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                int X = x0 + x, Y = y0 + y;
                if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                px[Y * w + X] = (((x / cell) + (y / cell)) & 1) ? 0xFFFF : c;
            }
        }
    }
}

void mg_kit_badge(const mg_team_t *t, uint16_t *px, int w, int h)
{
    if (t->league == MG_LEAGUE_NAT && t->flag[0]) {
        for (int i = 0; i < w * h; i++) px[i] = 0xFFFF;
        char buf[sizeof t->flag];
        memcpy(buf, t->flag, sizeof buf);
        char *save = buf;
        for (;;) {
            char *e = strchr(save, ';');
            if (e) *e = 0;
            stroke(save, px, w, h);
            if (!e) break;
            save = e + 1;
        }
        return;
    }
    /* a club: the shirt's design flattened, the front facing us */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f;      /* -1 left .. 1 right, as seen */
            float z = 1.0f - ((float)y + 0.5f) / (float)h;
            /* the front of the body: forward 1, left as seen is the wearer's right */
            float l = -u * 0.95f, f = sqrtf(1.0f - l * l * 0.9f);
            uint32_t c = mg_kit_shirt(t, f, l, z * 0.9f + 0.05f);
            if (t->design == MG_D_TRIM && (y < h / 6)) c = t->collar;
            px[y * w + x] = c565(c);
        }
    }
}

static float lum(uint32_t c)
{
    return 0.299f * (float)((c >> 16) & 255) + 0.587f * (float)((c >> 8) & 255) + 0.114f * (float)(c & 255);
}

static float dist(uint32_t a, uint32_t b)
{
    float dr = (float)((a >> 16) & 255) - (float)((b >> 16) & 255);
    float dg = (float)((a >> 8) & 255) - (float)((b >> 8) & 255);
    float db = (float)(a & 255) - (float)(b & 255);
    return sqrtf(dr * dr + dg * dg + db * db);
}

bool mg_kit_clash(const mg_team_t *a, const mg_team_t *b)
{
    return dist(a->c1, b->c1) < 90.0f || (dist(a->c1, b->c2) < 70.0f && dist(a->c2, b->c1) < 70.0f);
}

void mg_kit_away(const mg_team_t *t, mg_team_t *o)
{
    *o = *t;
    bool light = lum(t->c1) > 150.0f;
    uint32_t base = light ? 0x1E2230 : 0xF4F4F4;
    o->design = MG_D_TRIM;
    o->c2 = t->c1;
    o->c1 = base;
    o->sleeves = base;
    o->collar = t->c1;
    o->shorts = light ? 0x1E2230 : 0xF4F4F4;
    o->socks = base;
}
