/*
 * P4OS - Bolsa: what a symbol is and who serves it.
 *
 * The canonical symbol is the key everywhere (the lists, the cache, the
 * alerts, the page):
 *
 *   NANDU, YERBA.BA     a share in the United States / in Buenos Aires
 *   ^MERV, ^GSPC        an index
 *   cg:bitcoin          a coin, by its CoinGecko id
 *   USD.BLUE            a house of the Argentine dollar
 *   EUR.ARS             another currency, in pesos
 *   RIESGO.AR           the country risk
 *
 * The catalog below holds only what is not a company: the indices, the
 * coins, the dollar and the currencies. Shares are found with the
 * provider's own search (their names come from it, not from here) or typed
 * as a symbol.
 */
#include "bs.h"

typedef struct {
    const char *sym, *show, *name, *ccy, *remote;
    uint8_t kind;
} cat_t;

static const cat_t CAT[] = {
    /* Argentina, no key: ArgentinaDatos */
    { "USD.OFICIAL", N_("Oficial"),  N_("Dólar oficial"),          "ARS", "oficial",         BS_K_DOLAR },
    { "USD.BLUE",    N_("Blue"),     N_("Dólar blue"),             "ARS", "blue",            BS_K_DOLAR },
    { "USD.MEP",     N_("MEP"),      N_("Dólar MEP (bolsa)"),      "ARS", "bolsa",           BS_K_DOLAR },
    { "USD.CCL",     N_("CCL"),      N_("Dólar contado con liqui"),"ARS", "contadoconliqui", BS_K_DOLAR },
    { "USD.CRIPTO",  N_("Cripto"),   N_("Dólar cripto"),           "ARS", "cripto",          BS_K_DOLAR },
    { "USD.MAYOR",   N_("Mayorista"),N_("Dólar mayorista"),        "ARS", "mayorista",       BS_K_DOLAR },
    { "EUR.ARS",     "EUR",      N_("Euro en pesos"),          "ARS", "eur",             BS_K_FX },
    { "BRL.ARS",     "BRL",      N_("Real en pesos"),          "ARS", "brl",             BS_K_FX },
    { "CLP.ARS",     "CLP",      N_("Peso chileno en pesos"),  "ARS", "clp",             BS_K_FX },
    { "UYU.ARS",     "UYU",      N_("Peso uruguayo en pesos"), "ARS", "uyu",             BS_K_FX },
    { "RIESGO.AR",   N_("Riesgo"),   N_("Riesgo país"),            "",    "",                BS_K_RISK },
    /* Indices: a keyed provider */
    { "^MERV",  "MERV",  "Merval",      "ARS", "", BS_K_INDEX },
    { "^GSPC",  "SPX",   "S&P 500",     "USD", "", BS_K_INDEX },
    { "^DJI",   "DJI",   "Dow Jones",   "USD", "", BS_K_INDEX },
    { "^IXIC",  "IXIC",  "Nasdaq",      "USD", "", BS_K_INDEX },
    { "^BVSP",  "BVSP",  "Bovespa",     "BRL", "", BS_K_INDEX },
    { "^FTSE",  "FTSE",  "FTSE 100",    "GBP", "", BS_K_INDEX },
    { "^GDAXI", "DAX",   "DAX",         "EUR", "", BS_K_INDEX },
    { "^N225",  "N225",  "Nikkei 225",  "JPY", "", BS_K_INDEX },
    /* Coins: CoinGecko, no key */
    { "cg:bitcoin",  "BTC",  "Bitcoin",  "USD", "bitcoin",  BS_K_CRYPTO },
    { "cg:ethereum", "ETH",  "Ethereum", "USD", "ethereum", BS_K_CRYPTO },
    { "cg:solana",   "SOL",  "Solana",   "USD", "solana",   BS_K_CRYPTO },
    { "cg:ripple",   "XRP",  "XRP",      "USD", "ripple",   BS_K_CRYPTO },
    { "cg:cardano",  "ADA",  "Cardano",  "USD", "cardano",  BS_K_CRYPTO },
    { "cg:dogecoin", "DOGE", "Dogecoin", "USD", "dogecoin", BS_K_CRYPTO },
    { "cg:litecoin", "LTC",  "Litecoin", "USD", "litecoin", BS_K_CRYPTO },
    { "cg:polkadot", "DOT",  "Polkadot", "USD", "polkadot", BS_K_CRYPTO },
    { "cg:stellar",  "XLM",  "Stellar",  "USD", "stellar",  BS_K_CRYPTO },
    { "cg:monero",   "XMR",  "Monero",   "USD", "monero",   BS_K_CRYPTO },
};
#define NCAT ((int)(sizeof CAT / sizeof CAT[0]))

static void from_cat(const cat_t *c, bs_info_t *out)
{
    memset(out, 0, sizeof *out);
    bs_scpy(out->sym, sizeof out->sym, c->sym);
    /* the dollar's houses and the country risk are words, and translated */
    bs_scpy(out->show, sizeof out->show, c->kind == BS_K_DOLAR || c->kind == BS_K_RISK ? _(c->show) : c->show);
    /* the dollar's names are Spanish words and get translated */
    bs_scpy(out->name, sizeof out->name,
            c->kind == BS_K_DOLAR || c->kind == BS_K_FX || c->kind == BS_K_RISK ? _(c->name) : c->name);
    bs_scpy(out->ccy, sizeof out->ccy, c->ccy);
    out->kind = c->kind;
    out->remote = c->remote;
}

bool bs_sym_catalog(int i, bs_info_t *out)
{
    if (i < 0 || i >= NCAT) return false;
    from_cat(&CAT[i], out);
    return true;
}

static bool ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

void bs_sym_info(const char *sym, bs_info_t *out)
{
    for (int i = 0; i < NCAT; i++)
        if (!strcmp(CAT[i].sym, sym)) { from_cat(&CAT[i], out); return; }

    memset(out, 0, sizeof *out);
    bs_scpy(out->sym, sizeof out->sym, sym);
    const char *learned = bs_names_get(sym);
    if (!strncmp(sym, "cg:", 3)) {
        /* a coin found with the search: "SYM|Name" was learned */
        out->kind = BS_K_CRYPTO;
        out->remote = sym + 3;
        bs_scpy(out->ccy, sizeof out->ccy, "USD");
        const char *bar = learned ? strchr(learned, '|') : NULL;
        if (bar) {
            size_t k = (size_t)(bar - learned);
            if (k >= sizeof out->show) k = sizeof out->show - 1;
            memcpy(out->show, learned, k);
            out->show[k] = 0;
            bs_scpy(out->name, sizeof out->name, bar + 1);
        } else {
            bs_scpy(out->show, sizeof out->show, sym + 3);
            bs_scpy(out->name, sizeof out->name, sym + 3);
        }
        return;
    }
    out->kind = sym[0] == '^' ? BS_K_INDEX : BS_K_STOCK;
    bs_scpy(out->show, sizeof out->show, sym[0] == '^' ? sym + 1 : sym);
    char *dot = strstr(out->show, ".BA");
    if (dot && ends_with(out->show, ".BA")) *dot = 0;
    bs_scpy(out->ccy, sizeof out->ccy, ends_with(sym, ".BA") ? "ARS" : "USD");
    if (learned && learned[0]) {
        const char *bar = strchr(learned, '|');
        bs_scpy(out->name, sizeof out->name, bar ? bar + 1 : learned);
    } else {
        bs_scpy(out->name, sizeof out->name, ends_with(sym, ".BA") ? _("Bolsa de Buenos Aires") : "");
    }
}

const char *bs_prov_name(bs_prov_t p)
{
    switch (p) {
    case BS_P_COINGECKO: return "CoinGecko";
    case BS_P_ARGDATOS:  return "ArgentinaDatos";
    case BS_P_STOOQ:     return "Stooq";
    case BS_P_TWELVE:    return "Twelve Data";
    case BS_P_ALPHA:     return "Alpha Vantage";
    case BS_P_FINNHUB:   return "Finnhub";
    default:             return "";
    }
}

static bool keyed(bs_prov_t p) { return bs_cfg()->key[p][0] != 0; }

/* Stocks and indices: the preferred provider if it has a key and can do
 * it, else the first one with a key, in an order that puts the one that
 * does it best first. */
static bs_prov_t pick(const bs_info_t *in, bool hist)
{
    static const bs_prov_t STOCK_Q[] = { BS_P_TWELVE, BS_P_FINNHUB, BS_P_STOOQ, BS_P_ALPHA };
    static const bs_prov_t STOCK_H[] = { BS_P_TWELVE, BS_P_STOOQ, BS_P_ALPHA };
    static const bs_prov_t INDEX_Q[] = { BS_P_STOOQ, BS_P_TWELVE, BS_P_ALPHA };
    static const bs_prov_t INDEX_H[] = { BS_P_STOOQ, BS_P_TWELVE, BS_P_ALPHA };
    const bs_prov_t *ord;
    int n;
    bool idx = in->kind == BS_K_INDEX;
    if (hist) { ord = idx ? INDEX_H : STOCK_H; n = 3; }
    else if (idx) { ord = INDEX_Q; n = 3; }
    else { ord = STOCK_Q; n = 4; }
    /* the preference is for shares: indices are best where they are, and
     * the free plans of the others mostly do not carry them */
    bs_prov_t pref = idx ? BS_P_NONE : (bs_prov_t)bs_cfg()->prov;
    if (pref > BS_P_ARGDATOS && pref < BS_P_COUNT && keyed(pref))
        for (int i = 0; i < n; i++)
            if (ord[i] == pref) return pref;
    for (int i = 0; i < n; i++)
        if (keyed(ord[i])) return ord[i];
    return BS_P_NONE;
}

bs_prov_t bs_sym_prov_quote(const bs_info_t *in)
{
    switch (in->kind) {
    case BS_K_CRYPTO: return BS_P_COINGECKO;
    case BS_K_DOLAR: case BS_K_FX: case BS_K_RISK: return BS_P_ARGDATOS;
    default: return pick(in, false);
    }
}

bs_prov_t bs_sym_prov_hist(const bs_info_t *in)
{
    switch (in->kind) {
    case BS_K_CRYPTO: return BS_P_COINGECKO;
    case BS_K_DOLAR: case BS_K_FX: case BS_K_RISK: return BS_P_ARGDATOS;
    default: return pick(in, true);
    }
}

bool bs_sym_clean(const char *in, char *out, size_t n)
{
    size_t w = 0;
    bool lower_ok = !strncmp(in, "cg:", 3);
    for (const char *p = in; *p && w + 1 < n; p++) {
        char c = *p;
        if (!lower_ok && c >= 'a' && c <= 'z') c = (char)(c - 32);
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '^' ||
                  c == '-' || c == ':' || (lower_ok && c >= 'a' && c <= 'z');
        if (ok) out[w++] = c;
    }
    out[w] = 0;
    return w > 0;
}
