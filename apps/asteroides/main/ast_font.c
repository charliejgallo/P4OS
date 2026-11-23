/*
 * DERIVA - the vector font: capitals drawn as strokes on a grid of
 * 4 x 6 units (y down), the digits and a few signs, and the accents of the
 * Latin languages laid over them, so a banner in German or Portuguese is
 * drawn with the same light as the rocks. Lower case is drawn as upper
 * case. A glyph is a string of polylines: two digits a point (x then y),
 * a space between polylines.
 */
#include "ast_gfx.h"

#include <math.h>
#include <string.h>

typedef struct {
    uint16_t cp;
    const char *s;
} glyph_t;

static const glyph_t GLYPHS[] = {
    { 'A', "0602204246 0444" },
    { 'B', "06003041423344453606 0333" },
    { 'C', "4130100105163645" },
    { 'D', "00304145360600" },
    { 'E', "40000646 0333" },
    { 'F', "400006 0333" },
    { 'G', "41301001051636454323" },
    { 'H', "0006 4046 0343" },
    { 'I', "1030 2026 1636" },
    { 'J', "4045361605" },
    { 'K', "0006 4004 1346" },
    { 'L', "000646" },
    { 'M', "0600234046" },
    { 'N', "06004640" },
    { 'O', "103041453616050110" },
    { 'P', "06003041423303" },
    { 'Q', "103041453616050110 2446" },
    { 'R', "06003041423303 2346" },
    { 'S', "413010010213334445361605" },
    { 'T', "0040 2026" },
    { 'U', "000516364540" },
    { 'V', "002640" },
    { 'W', "0016233640" },
    { 'X', "0046 4006" },
    { 'Y', "002340 2326" },
    { 'Z', "00400646" },
    { '0', "103041453616050110 3115" },
    { '1', "112026 1636" },
    { '2', "01103041420646" },
    { '3', "0110304142334445361605 1333" },
    { '4', "36300444" },
    { '5', "4000033344453606" },
    { '6', "30100105163645443303" },
    { '7', "004016" },
    { '8', "103041423313020110 1304051636454433" },
    { '9', "43130201103041453616" },
    { '.', "2526" },
    { ',', "2517" },
    { ':', "2122 2526" },
    { ';', "2122 2517" },
    { '!', "2023 2526" },
    { 0xA1, "2021 2326" },          /* ¡ */
    { '?', "01103041422324 2526" },
    { 0xBF, "2021 22230405163645" }, /* ¿ */
    { '-', "1333" },
    { '+', "0343 2125" },
    { '/', "0640" },
    { '\'', "2021" },
    { '"', "1112 3132" },
    { '(', "30212536" },
    { ')', "10212516" },
    { '%', "0640 0011 3546" },
    { 0xD7, "1234 3214" },          /* × */
    { 0xB7, "2324" },               /* · */
    { '=', "0242 0444" },
    { '<', "400244" },
    { '>', "004206" },
    { '*', "2125 0442 0244" },
    { '#', "1016 3036 0242 0444" },
    { 0xB0, "1020313221120110" },   /* ° */
};

/* the accents, above a capital (y < 0) or under it */
enum { AC_NONE, AC_ACUTE, AC_GRAVE, AC_CIRC, AC_DIAER, AC_TILDE, AC_RING, AC_CEDIL };

static const float AC_ACUTE_P[] = { 1.5f, -1.0f, 2.8f, -2.3f };
static const float AC_GRAVE_P[] = { 1.2f, -2.3f, 2.5f, -1.0f };
static const float AC_CIRC_P[]  = { 0.9f, -1.0f, 2.0f, -2.2f, 3.1f, -1.0f };
static const float AC_TILDE_P[] = { 0.4f, -1.2f, 1.4f, -2.1f, 2.6f, -1.2f, 3.6f, -2.1f };
static const float AC_RING_P[]  = { 2.0f, -2.6f, 2.7f, -1.8f, 2.0f, -1.0f, 1.3f, -1.8f, 2.0f, -2.6f };
static const float AC_CEDIL_P[] = { 2.0f, 6.0f, 2.0f, 6.9f, 1.1f, 7.6f };

typedef struct {
    uint16_t cp;
    char base;
    uint8_t ac;
} accented_t;

static const accented_t ACCENTED[] = {
    { 0xC0, 'A', AC_GRAVE }, { 0xC1, 'A', AC_ACUTE }, { 0xC2, 'A', AC_CIRC }, { 0xC3, 'A', AC_TILDE },
    { 0xC4, 'A', AC_DIAER }, { 0xC5, 'A', AC_RING },  { 0xC7, 'C', AC_CEDIL }, { 0xC8, 'E', AC_GRAVE },
    { 0xC9, 'E', AC_ACUTE }, { 0xCA, 'E', AC_CIRC },  { 0xCB, 'E', AC_DIAER }, { 0xCC, 'I', AC_GRAVE },
    { 0xCD, 'I', AC_ACUTE }, { 0xCE, 'I', AC_CIRC },  { 0xCF, 'I', AC_DIAER }, { 0xD1, 'N', AC_TILDE },
    { 0xD2, 'O', AC_GRAVE }, { 0xD3, 'O', AC_ACUTE }, { 0xD4, 'O', AC_CIRC },  { 0xD5, 'O', AC_TILDE },
    { 0xD6, 'O', AC_DIAER }, { 0xD9, 'U', AC_GRAVE }, { 0xDA, 'U', AC_ACUTE }, { 0xDB, 'U', AC_CIRC },
    { 0xDC, 'U', AC_DIAER },
};

static uint32_t next_cp(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xF0 && s[1] && s[2] && s[3]) {
        c = ((c & 7) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u);
        n = 4;
    } else if (c >= 0xE0 && s[1] && s[2]) {
        c = ((c & 15) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
        n = 3;
    } else if (c >= 0xC0 && s[1]) {
        c = ((c & 31) << 6) | (s[1] & 63u);
        n = 2;
    }
    *ps += n;
    return c;
}

/* lower case to upper, in ASCII and Latin-1 */
static uint32_t upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0x2019 || c == 0x2018) return '\'';
    if (c == 0x201C || c == 0x201D || c == 0xAB || c == 0xBB) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    if (c == 0xA0) return ' ';
    return c;
}

static const char *glyph(uint32_t c)
{
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++) {
        if (GLYPHS[i].cp == c) return GLYPHS[i].s;
    }
    return NULL;
}

/* how wide a character is, in units, and what it is drawn as */
static float adv_of(uint32_t c)
{
    if (c == ' ') return 4.0f;
    if (c == 'I' || c == '1' || c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == 0xA1 || c == '\'' ||
        c == 0xB7)
        return 4.0f;
    if (c == 0xDF) return 12.0f;    /* ß: SS */
    return 6.0f;
}

static void stroke_str(ast_gfx_t *g, const char *s, float x, float y, float u, uint32_t rgb, int k)
{
    float w = u / 6.0f;
    if (w < 0.7f) w = 0.7f;
    while (*s) {
        while (*s == ' ') s++;
        float px = 0, py = 0;
        int n = 0;
        while (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') {
            float qx = x + (float)(s[0] - '0') * u, qy = y + (float)(s[1] - '0') * u;
            if (n) ast_line(g, px, py, qx, qy, rgb, w, k, AST_F_CAPS);
            px = qx;
            py = qy;
            n++;
            s += 2;
        }
        if (n == 1) ast_line(g, px, py, px, py + 0.01f, rgb, w, k, AST_F_CAPS);
        if (*s && *s != ' ') s++;
    }
}

static void stroke_pts(ast_gfx_t *g, const float *p, int n, float x, float y, float u, uint32_t rgb, int k)
{
    float w = u / 6.0f;
    if (w < 0.7f) w = 0.7f;
    for (int i = 0; i + 1 < n; i++)
        ast_line(g, x + p[i * 2] * u, y + p[i * 2 + 1] * u, x + p[i * 2 + 2] * u, y + p[i * 2 + 3] * u, rgb, w, k,
                 AST_F_CAPS);
}

static void accent(ast_gfx_t *g, int ac, float x, float y, float u, uint32_t rgb, int k)
{
    switch (ac) {
    case AC_ACUTE: stroke_pts(g, AC_ACUTE_P, 2, x, y, u, rgb, k); break;
    case AC_GRAVE: stroke_pts(g, AC_GRAVE_P, 2, x, y, u, rgb, k); break;
    case AC_CIRC:  stroke_pts(g, AC_CIRC_P, 3, x, y, u, rgb, k); break;
    case AC_TILDE: stroke_pts(g, AC_TILDE_P, 4, x, y, u, rgb, k); break;
    case AC_RING:  stroke_pts(g, AC_RING_P, 5, x, y, u, rgb, k); break;
    case AC_CEDIL: stroke_pts(g, AC_CEDIL_P, 3, x, y, u, rgb, k); break;
    case AC_DIAER: {
        static const float L[] = { 1.0f, -2.0f, 1.0f, -1.1f };
        static const float R[] = { 3.0f, -2.0f, 3.0f, -1.1f };
        stroke_pts(g, L, 2, x, y, u, rgb, k);
        stroke_pts(g, R, 2, x, y, u, rgb, k);
        break;
    }
    default: break;
    }
}

float ast_text_width(const char *s, float u)
{
    float w = 0;
    while (*s) {
        uint32_t c = upper(next_cp(&s));
        w += adv_of(c) * u;
    }
    return w > 0 ? w - 2.0f * u : 0;
}

float ast_text(ast_gfx_t *g, float x, float y, const char *s, float u, uint32_t rgb, int k, int flags)
{
    float w = ast_text_width(s, u);
    if (flags & AST_F_CENTER) x -= w / 2;
    else if (flags & AST_F_RIGHT) x -= w;
    while (*s) {
        uint32_t c = upper(next_cp(&s));
        float adv = adv_of(c);
        /* the narrow ones sit in the middle of their 4 units */
        float ox = adv == 4.0f && c != ' ' ? -1.0f * u : 0;
        if (c == 0xDF) {
            stroke_str(g, glyph('S'), x, y, u, rgb, k);
            stroke_str(g, glyph('S'), x + 6 * u, y, u, rgb, k);
        } else {
            const char *gl = glyph(c);
            if (!gl) {
                for (size_t i = 0; i < sizeof ACCENTED / sizeof ACCENTED[0]; i++) {
                    if (ACCENTED[i].cp == c) {
                        gl = glyph((uint32_t)ACCENTED[i].base);
                        accent(g, ACCENTED[i].ac, x, y, u, rgb, k);
                        break;
                    }
                }
            }
            if (gl) stroke_str(g, gl, x + ox, y, u, rgb, k);
        }
        x += adv * u;
    }
    return w;
}
