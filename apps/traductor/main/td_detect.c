/*
 * P4OS - Traductor: which language a text is in.
 *
 * Offline and small: each language's most common short words, and a few
 * letters only it uses. A word counts once per occurrence; letters such as
 * ñ, ç, ß or ã tip the balance. Good for a sentence; a single word may come
 * out as NULL, and the app then keeps what was chosen.
 */
#include "td.h"

typedef struct {
    const char *code;
    const char *words;      /* space-separated, lowercase                    */
    const char *marks;      /* UTF-8 letters typical of it, space-separated  */
} profile_t;

static const profile_t P[] = {
    {"es", "el la los las de del que y en un una por con para es no se lo su al como más pero sus le ya o este sí porque esta entre cuando muy sin sobre también me hasta hay donde quien desde todo nos durante todos uno les ni contra otros ese eso ante ellos e esto mí antes algunos qué unos yo otro otras otra él tanto esa estos mucho quienes nada muchos cual poco ella estar estas algunas algo nosotros mi mis tú te ti tu tus ellas vos sos está estás estoy hola gracias dónde cómo",
     "ñ ¿ ¡ ó á í ú é"},
    {"en", "the of and to in is you that it he was for on are as with his they i at be this have from or one had by word but not what all were we when your can said there use an each which she do how their if will up other about out many then them these so some her would make like him into time has look two more write go see number no way could people my than first water been call who oil its now find long down day did get come made may part i'm don't it's where thanks hello please",
     ""},
    {"pt", "o a os as de do da dos das que e em um uma por com para é não se no na nos nas ao como mais mas seu sua ou ser quando muito há também já está eu tem só pelo pela até isso ela entre era depois sem mesmo aos seus quem nas me esse eles você essa num nem suas meu às minha numa pelos elas qual nós lhe deles essas esses pelas este dele tu te vocês obrigado olá onde",
     "ã õ ç ê â"},
    {"it", "il lo la i gli le di da in con su per tra fra un uno una e è che non si del della dei delle al alla ai alle nel nella sono come anche ma più questo questa mi ti ci vi ne lui lei noi voi loro mio tuo suo cosa quando dove perché ho hai ha abbiamo avete hanno essere sei siamo siete grazie ciao buongiorno",
     "ò ì è"},
    {"fr", "le la les de des du un une et en est que qui dans pour pas au aux il elle ils elles nous vous je tu on ce cette ces son sa ses sur avec ne se par plus mais ou où comme être avoir fait sont été mon ma mes ton ta tes leur leurs très bien aussi merci bonjour oui non c'est j'ai",
     "ç à è ê â û ô œ"},
    {"de", "der die das und ist nicht ein eine einen zu den von mit sich des auf für im dem es an auch als nach wie bei aus er sie wir ihr ich du oder aber wenn noch nur so hat haben sind war wird werden kann mein dein sein ihre unser über vor durch zum zur danke hallo bitte ja nein",
     "ß ä ö ü"},
    {"ca", "el la els les de del que i en un una per amb no es és com més però al als també hi ha seu seva molt ser on quan aquest aquesta jo tu ell ella nosaltres vosaltres ells elles meu teu gràcies hola",
     "ç à è ò l·l"},
    {"nl", "de het een en van in is dat op te zijn met voor niet aan er die maar om ook als dan bij of uit nog wel naar heeft hebben kan wat hij zij wij jullie ik je mijn dank hallo",
     "ĳ"},
};
#define NP ((int)(sizeof P / sizeof P[0]))

static bool word_in(const char *list, const char *w, size_t wl)
{
    const char *s = list;
    while (*s) {
        const char *e = strchr(s, ' ');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == wl && !memcmp(s, w, wl)) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

/* lowercase ASCII and the Latin-1 capitals (UTF-8 C3 80-9E) */
static void lower(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (unsigned char)(*p + 32);
        else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            p[1] = (unsigned char)(p[1] + 0x20);
            p++;
        }
    }
}

static bool is_sep(unsigned char c)
{
    return c <= ' ' || c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '"' ||
           c == '(' || c == ')' || c == '-';
}

const char *td_detect(const char *text, const char *const *codes, int n)
{
    if (n <= 0) return NULL;
    size_t tl = strlen(text);
    if (tl > 1500) tl = 1500;
    char *t = (char *)malloc(tl + 1);
    if (!t) return NULL;
    memcpy(t, text, tl);
    t[tl] = 0;
    lower(t);
    float score[NP] = {0};
    int words = 0;
    for (size_t i = 0; i < tl;) {
        while (i < tl && is_sep((unsigned char)t[i])) {
            /* the opening marks of Spanish are separators but tell a lot */
            i++;
        }
        size_t j = i;
        while (j < tl && !is_sep((unsigned char)t[j])) j++;
        if (j > i) {
            words++;
            for (int p = 0; p < NP; p++)
                if (word_in(P[p].words, t + i, j - i)) score[p] += 1.0f;
        }
        i = j;
    }
    for (int p = 0; p < NP; p++) {
        const char *m = P[p].marks;
        while (*m) {
            const char *e = strchr(m, ' ');
            size_t l = e ? (size_t)(e - m) : strlen(m);
            char mk[8];
            if (l < sizeof mk) {
                memcpy(mk, m, l);
                mk[l] = 0;
                for (const char *f = strstr(t, mk); f; f = strstr(f + l, mk)) score[p] += 0.7f;
            }
            if (!e) break;
            m = e + 1;
        }
    }
    free(t);
    int best = -1, second = -1;
    for (int p = 0; p < NP; p++) {
        bool allowed = false;
        for (int k = 0; k < n; k++)
            if (!strcmp(codes[k], P[p].code)) allowed = true;
        if (!allowed) continue;
        if (best < 0 || score[p] > score[best]) {
            second = best;
            best = p;
        } else if (second < 0 || score[p] > score[second]) {
            second = p;
        }
    }
    (void)words;
    if (best < 0 || score[best] < 1.0f) return NULL;
    if (second >= 0 && score[best] - score[second] < 0.5f) return NULL;
    return P[best].code;
}
