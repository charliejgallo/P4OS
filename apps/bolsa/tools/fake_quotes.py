#!/usr/bin/env python3
"""
Fake market-data server for the Bolsa app (apps/bolsa).

One HTTP server that answers in the shape of every provider the app knows,
each under its own prefix, so the app is tested without touching the real
services:

    /coingecko/api/v3/...          CoinGecko keyless (crypto)
    /argdatos/v1/...               ArgentinaDatos (dollar, currencies, country risk)
    /stooq/q/d/l/?s=...            Stooq daily CSV (wants an apikey since 2026)
    /twelve/...                    Twelve Data (time_series, quote, symbol_search)
    /alpha/query?function=...      Alpha Vantage
    /finnhub/api/v1/...            Finnhub (quote, search)

Start the simulator with BOLSA_FAKE=http://127.0.0.1:8248 and the app swaps
each provider's real base URL for this server plus the prefix.

Every series is made up: a random walk seeded by the symbol, so the same
symbol always draws the same chart, ending today at a believable level. The
companies are invented too (see COMPANIES): no real names in tests or
screenshots.

Errors on purpose:
    symbol ERR500     the provider answers 500
    symbol SLOW       the answer takes 25 s (the app's timeout and retry)
    symbol BAD        a body that is not what the provider sends
    symbol EMPTY      a valid answer with no data
    apikey/token bad  the provider's "wrong key" answer
    --strict          the real rate limits: CoinGecko ~5/min, Twelve Data
                      8/min, Alpha Vantage 25/day (as 5/min here), Finnhub
                      60/min; past them, each provider's own limit answer
    --no-key-stooq    Stooq without an apikey answers like the real one now

GET /_stats returns how many requests each provider got (for the tests).
"""
import argparse
import hashlib
import json
import math
import random
import time
import threading
from datetime import datetime, timedelta, timezone, date
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ARGS = None
STATS = {}
CALLS = {}          # provider -> list of timestamps (rate limits)
LOCK = threading.Lock()

# ---------------------------------------------------------------------------
# What the fake markets hold
# ---------------------------------------------------------------------------

# Invented companies: symbol -> (name, exchange, currency, price today)
COMPANIES = {
    "YERBA":    ("Yerba Norte S.A.",        "BCBA",   "ARS", 2480.0),
    "ALFA":     ("Alfajores del Plata",     "BCBA",   "ARS", 1835.0),
    "VIENTO":   ("Viento Sur Energía",      "BCBA",   "ARS", 960.0),
    "MATE":     ("Mate Energía ADR",        "NYSE",   "USD", 44.8),
    "NANDU":    ("Ñandú Tecnología",        "NASDAQ", "USD", 182.4),
    "ACME":     ("Acme Robots",             "NASDAQ", "USD", 318.2),
    "CNDR":     ("Cóndor Aerolíneas",       "NYSE",   "USD", 12.35),
    "TANGO":    ("Tango Banco ADR",         "NYSE",   "USD", 61.7),
    "PAMPA":    ("Pampa Agro",              "NYSE",   "USD", 27.9),
    "NANDUC":   ("Ñandú Tecnología CEDEAR", "BCBA",   "ARS", 15900.0),
}

INDICES = {
    "^GSPC": ("S&P 500", 6110.0), "^DJI": ("Dow Jones", 44350.0),
    "^IXIC": ("Nasdaq", 20120.0), "^MERV": ("Merval", 2185000.0),
    "^BVSP": ("Bovespa", 131500.0), "^FTSE": ("FTSE 100", 8420.0),
    "^GDAXI": ("DAX", 20350.0), "^N225": ("Nikkei 225", 39800.0),
}

COINS = {   # CoinGecko id -> (symbol, name, price)
    "bitcoin": ("btc", "Bitcoin", 96300.0), "ethereum": ("eth", "Ethereum", 3480.0),
    "solana": ("sol", "Solana", 212.0), "ripple": ("xrp", "XRP", 2.31),
    "cardano": ("ada", "Cardano", 0.93), "dogecoin": ("doge", "Dogecoin", 0.318),
    "litecoin": ("ltc", "Litecoin", 108.0), "polkadot": ("dot", "Polkadot", 7.1),
    "stellar": ("xlm", "Stellar", 0.41), "monero": ("xmr", "Monero", 182.0),
}

DOLAR = {"oficial": 1452.0, "blue": 1485.0, "bolsa": 1503.0, "contadoconliqui": 1521.0,
         "cripto": 1512.0, "mayorista": 1418.0, "solidario": 1887.0, "tarjeta": 1887.0,
         "turista": 1887.0}
MONEDAS = {"usd": 1452.0, "eur": 1688.0, "brl": 268.5, "clp": 1.55, "uyu": 36.2}

# Stooq's own names for the indices (theirs, as far as is known: not checked
# against the live service, which now wants a key)
STOOQ_IDX = {"^spx": "^GSPC", "^dji": "^DJI", "^ndq": "^IXIC", "^mrv": "^MERV",
             "^bvp": "^BVSP", "^ukx": "^FTSE", "^dax": "^GDAXI", "^nkx": "^N225"}

# ---------------------------------------------------------------------------
# Series
# ---------------------------------------------------------------------------

def rng_for(name, salt=""):
    h = hashlib.sha256((name + "|" + salt).encode()).digest()
    return random.Random(int.from_bytes(h[:8], "big"))


def today_utc():
    return datetime.now(timezone.utc).date()


def daily_series(name, end_price, start=date(2004, 1, 2), vol=0.018, drift=0.0003,
                 weekdays_only=True, volume=2e6):
    """Daily OHLCV from 'start' up to and including today, ending at end_price
    (today's close moves a little with the minute, so a refresh sees a new
    price)."""
    r = rng_for(name)
    days = []
    d = start
    t = today_utc()
    while d <= t:
        if not weekdays_only or d.weekday() < 5:
            days.append(d)
        d += timedelta(days=1)
    n = len(days)
    # walk in log space backwards from the end so it ends where it should
    rets = [r.gauss(drift, vol) for _ in range(n)]
    logs = [0.0] * n
    logs[-1] = math.log(end_price)
    for i in range(n - 2, -1, -1):
        logs[i] = logs[i + 1] - rets[i + 1]
    out = []
    minute = int(time.time() // 60)
    jr = rng_for(name, str(minute))
    for i, d in enumerate(days):
        c = math.exp(logs[i])
        if i == n - 1:
            c *= 1 + jr.gauss(0, vol / 6)          # today's price moves
        o = math.exp(logs[i - 1]) * (1 + r.gauss(0, vol / 4)) if i else c
        hi = max(o, c) * (1 + abs(r.gauss(0, vol / 2)))
        lo = min(o, c) * (1 - abs(r.gauss(0, vol / 2)))
        v = int(volume * math.exp(r.gauss(0, 0.45)))
        out.append((d, o, hi, lo, c, v))
    return out


def intraday(name, prev_close, last_close, minutes=5, bars=78, crypto=False, vol=0.0025):
    """Bars of 'minutes' ending at the last mark before now, from prev_close
    to last_close. Stocks get one session (13:30-20:00 UTC) of the latest
    weekday; crypto, the last 'bars' bars around the clock."""
    now = datetime.now(timezone.utc).replace(second=0, microsecond=0)
    now -= timedelta(minutes=now.minute % minutes)
    if crypto:
        times = [now - timedelta(minutes=minutes * (bars - 1 - i)) for i in range(bars)]
    else:
        d = now
        while d.weekday() >= 5:
            d -= timedelta(days=1)
        open_t = d.replace(hour=13, minute=30)
        close_t = d.replace(hour=20, minute=0)
        if now < open_t:      # before today's open: yesterday's session
            d -= timedelta(days=1)
            while d.weekday() >= 5:
                d -= timedelta(days=1)
            open_t = d.replace(hour=13, minute=30)
            close_t = d.replace(hour=20, minute=0)
        end = min(now, close_t - timedelta(minutes=minutes))
        times = []
        t = open_t
        while t <= end:
            times.append(t)
            t += timedelta(minutes=minutes)
        times = times[-bars:] if bars else times
    r = rng_for(name, "intra%d" % minutes)
    n = max(1, len(times))
    steps = [r.gauss(0, vol) for _ in range(n)]
    total = math.log(last_close / prev_close)
    # bend the walk so it lands on last_close
    s = 0.0
    logs = []
    for i in range(n):
        s += steps[i]
        logs.append(s)
    bend = (total - logs[-1]) / n
    out = []
    p = prev_close
    for i, t in enumerate(times):
        c = prev_close * math.exp(logs[i] + bend * (i + 1))
        o = p
        hi = max(o, c) * (1 + abs(r.gauss(0, vol / 3)))
        lo = min(o, c) * (1 - abs(r.gauss(0, vol / 3)))
        v = int(30000 * math.exp(r.gauss(0, 0.6)))
        out.append((t, o, hi, lo, c, v))
        p = c
    return out


def lookup(sym):
    """symbol -> (name, currency, price, daily kwargs)"""
    s = sym.upper()
    if s.endswith(".BA"):
        s = s[:-3]
    if s in COMPANIES:
        n, ex, cur, p = COMPANIES[s]
        return n, cur, p, {"volume": 3e5 if cur == "ARS" else 4e6,
                           "vol": 0.026 if cur == "ARS" else 0.017}
    if s in INDICES:
        n, p = INDICES[s]
        return n, "USD", p, {"vol": 0.011, "volume": 0, "start": date(1996, 1, 2)}
    return None


# ---------------------------------------------------------------------------
# Rate limits and errors
# ---------------------------------------------------------------------------

LIMITS = {"coingecko": (5, 60), "twelve": (8, 60), "alpha": (5, 60), "finnhub": (60, 60),
          "stooq": (30, 60), "argdatos": (60, 60)}


def over_limit(prov, cost=1):
    if not ARGS.strict:
        return False
    n, win = LIMITS[prov]
    now = time.time()
    with LOCK:
        lst = [t for t in CALLS.get(prov, []) if now - t < win]
        if len(lst) + cost > n:
            CALLS[prov] = lst
            return True
        lst += [now] * cost
        CALLS[prov] = lst
    return False


# ---------------------------------------------------------------------------
# The handler
# ---------------------------------------------------------------------------

def f2(x, nd=4):
    return float("%.*f" % (nd, x))


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        if not ARGS.quiet:
            super().log_message(fmt, *a)

    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def special(self, sym):
        s = (sym or "").upper()
        if "ERR500" in s:
            self.send(500, {"error": "internal"})
            return True
        if "SLOW" in s:
            time.sleep(25)
        if "BAD" in s and "BADGE" not in s:
            self.send(200, "<html>this is not data</html>", "text/html")
            return True
        return False

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        parts = [p for p in u.path.split("/") if p]
        if not parts:
            self.send(404, {"error": "no route"})
            return
        prov = parts[0]
        with LOCK:
            STATS[prov] = STATS.get(prov, 0) + 1
        try:
            if prov == "_stats":
                self.send(200, STATS)
            elif prov == "coingecko":
                self.coingecko(parts[1:], q)
            elif prov == "argdatos":
                self.argdatos(parts[1:], q)
            elif prov == "stooq":
                self.stooq(parts[1:], q)
            elif prov == "twelve":
                self.twelve(parts[1:], q)
            elif prov == "alpha":
                self.alpha(q)
            elif prov == "finnhub":
                self.finnhub(parts[1:], q)
            else:
                self.send(404, {"error": "no route"})
        except BrokenPipeError:
            pass

    # ---- CoinGecko (keyless public API) ----
    def coingecko(self, parts, q):
        if over_limit("coingecko"):
            self.send(429, {"status": {"error_code": 429,
                                       "error_message": "You've exceeded the Rate Limit."}})
            return
        path = "/".join(parts)
        if path == "api/v3/simple/price":
            ids = [i for i in q.get("ids", "").split(",") if i]
            out = {}
            for i in ids:
                if self.special(i):
                    return
                if i not in COINS:
                    continue
                s = daily_series(i, COINS[i][2], start=date(2015, 1, 1), vol=0.035,
                                 weekdays_only=False, volume=2e10)
                c, prev = s[-1][4], s[-2][4]
                e = {"usd": f2(c, 6 if c < 1 else 2)}
                if q.get("include_24hr_change") == "true":
                    e["usd_24h_change"] = (c / prev - 1) * 100
                if q.get("include_24hr_vol") == "true":
                    e["usd_24h_vol"] = s[-1][5] * 1.0
                if q.get("include_last_updated_at") == "true":
                    e["last_updated_at"] = int(time.time()) - 40
                out[i] = e
            self.send(200, out)
        elif path == "api/v3/search":
            qq = q.get("query", "").lower()
            coins = [{"id": k, "name": v[1], "symbol": v[0].upper(), "market_cap_rank": n + 1}
                     for n, (k, v) in enumerate(COINS.items())
                     if qq and (qq in k or qq in v[1].lower() or qq == v[0])]
            self.send(200, {"coins": coins, "exchanges": [], "categories": []})
        elif len(parts) == 5 and parts[:3] == ["api", "v3", "coins"] and parts[4] == "market_chart":
            cid = parts[3]
            if self.special(cid):
                return
            if cid not in COINS:
                self.send(404, {"error": "coin not found"})
                return
            days = q.get("days", "1")
            s = daily_series(cid, COINS[cid][2], start=date(2015, 1, 1), vol=0.035,
                             weekdays_only=False, volume=2e10)
            nd = 100000 if days == "max" else int(days)
            if nd <= 1:
                bars = intraday(cid, s[-2][4], s[-1][4], 5, 288, crypto=True, vol=0.003)
                pts = [(int(x[0].timestamp() * 1000), x[4], x[5] * 300) for x in bars]
            elif nd <= 90:
                bars = intraday(cid + "h", s[-nd - 1][4], s[-1][4], 60, nd * 24, crypto=True,
                                vol=0.007)
                pts = [(int(x[0].timestamp() * 1000), x[4], x[5] * 2000) for x in bars]
            else:
                # the keyless and demo plans give a year at most
                ss = s[-min(nd, 365) - 1:]
                pts = [(int(datetime(d.year, d.month, d.day, tzinfo=timezone.utc).timestamp() * 1000),
                        c, v) for d, o, h, l, c, v in ss]
            self.send(200, {
                "prices": [[t, p] for t, p, v in pts],
                "market_caps": [[t, p * 19e6] for t, p, v in pts],
                "total_volumes": [[t, v] for t, p, v in pts],
            })
        else:
            self.send(404, {"error": "Incorrect path. Please check https://www.coingecko.com/api/"})

    # ---- ArgentinaDatos ----
    def argdatos(self, parts, q):
        path = "/".join(parts)
        if path.startswith("v1/cotizaciones/dolares/"):
            casa = parts[3]
            if casa not in DOLAR:
                self.send(404, {"error": "casa no encontrada"})
                return
            self.send(200, self.ad_hist(casa, DOLAR[casa], "USD", casa))
        elif path.startswith("v1/cotizaciones/") and len(parts) == 3:
            m = parts[2]
            if m not in MONEDAS:
                self.send(404, {"error": "moneda no encontrada"})
                return
            self.send(200, self.ad_hist("mon" + m, MONEDAS[m], m.upper(), "oficial"))
        elif path == "v1/finanzas/indices/riesgo-pais":
            self.send(200, self.risk())
        elif path == "v1/finanzas/indices/riesgo-pais/ultimo":
            self.send(200, self.risk()[-1])
        else:
            self.send(404, {"error": "ruta"})

    def ad_hist(self, name, end, moneda, casa):
        s = daily_series("ad" + name, end, start=date(2011, 1, 3), vol=0.008, drift=0.0012,
                         weekdays_only=True)
        out = []
        for d, o, h, l, c, v in s:
            venta = round(c, 2)
            compra = round(c * 0.97, 2) if casa not in ("tarjeta", "solidario", "turista") else None
            out.append({"moneda": moneda, "casa": casa, "compra": compra, "venta": venta,
                        "fecha": d.isoformat()})
        return out

    def risk(self):
        s = daily_series("riesgo", 652, start=date(1999, 1, 4), vol=0.022, drift=-0.0001,
                         weekdays_only=True)
        return [{"fecha": d.isoformat(), "valor": int(round(c))} for d, o, h, l, c, v in s]

    # ---- Stooq ----
    def stooq(self, parts, q):
        if parts[:3] != ["q", "d", "l"]:
            self.send(404, "Not found", "text/plain")
            return
        if ARGS.no_key_stooq and not q.get("apikey"):
            self.send(200, "Get your apikey: https://stooq.com/q/d/?s=&get_apikey\n", "text/plain")
            return
        if q.get("apikey") == "bad":
            self.send(200, "Access denied\n", "text/plain")
            return
        if over_limit("stooq"):
            self.send(200, "Exceeded the daily hits limit\n", "text/plain")
            return
        s = q.get("s", "").lower()
        if self.special(s):
            return
        if s in STOOQ_IDX:
            sym = STOOQ_IDX[s]
        elif s.endswith(".us"):
            sym = s[:-3].upper()
        elif s.endswith(".ar"):
            sym = s[:-3].upper() + ".BA"
        else:
            sym = s.upper()
        info = lookup(sym)
        if not info or "EMPTY" in sym:
            self.send(200, "No data\n", "text/plain")
            return
        name, cur, price, kw = info
        ser = daily_series(sym, price, **kw)
        if q.get("d1"):
            d1 = datetime.strptime(q["d1"], "%Y%m%d").date()
            ser = [x for x in ser if x[0] >= d1]
        lines = ["Date,Open,High,Low,Close,Volume"]
        for d, o, h, l, c, v in ser:
            lines.append("%s,%.4g,%.4g,%.4g,%.4g,%d" % (d.isoformat(), o, h, l, c, v) if c < 1
                         else "%s,%.2f,%.2f,%.2f,%.2f,%d" % (d.isoformat(), o, h, l, c, v))
        self.send(200, "\r\n".join(lines) + "\r\n", "text/csv")

    # ---- Twelve Data ----
    def twelve(self, parts, q):
        key = q.get("apikey", "")
        if not key or key == "bad":
            self.send(200, {"code": 401, "message": "**apikey** parameter is incorrect or not specified. You can get your free API key instantly following this link: https://twelvedata.com/pricing.", "status": "error"})
            return
        syms = [s for s in q.get("symbol", "").split(",") if s]
        ep = parts[0] if parts else ""
        if ep != "symbol_search" and over_limit("twelve", max(1, len(syms))):
            self.send(200, {"code": 429, "message": "You have run out of API credits for the current minute. %d API credits were used, with the current limit being 8. Wait for the next minute or consider switching to a higher tier plan at https://twelvedata.com/pricing" % (8 + len(syms)), "status": "error"})
            return
        if ep == "symbol_search":
            qq = q.get("symbol", "").upper()
            data = []
            for k, (n, ex, cur, p) in COMPANIES.items():
                if qq and (qq in k or qq in n.upper()):
                    data.append({"symbol": k, "instrument_name": n, "exchange": ex,
                                 "mic_code": "XBUE" if ex == "BCBA" else "XNYS", "exchange_timezone": "America/New_York",
                                 "instrument_type": "Common Stock", "country": "Argentina" if ex == "BCBA" else "United States",
                                 "currency": cur})
            self.send(200, {"data": data, "status": "ok"})
            return
        if ep == "quote":
            out = {}
            for s in syms:
                out[s] = self.tw_quote(s)
            self.send(200, out[syms[0]] if len(syms) == 1 else out)
            return
        if ep == "time_series":
            if len(syms) != 1:
                self.send(200, {"code": 400, "message": "one symbol at a time here", "status": "error"})
                return
            s = syms[0]
            if self.special(s):
                return
            info = lookup(self.tw_sym(s, q.get("exchange")))
            if not info:
                self.send(200, {"code": 404, "message": "**symbol** not found: %s. Please specify it correctly according to API Documentation." % s, "status": "error"})
                return
            name, cur, price, kw = info
            ser = daily_series(self.tw_sym(s, q.get("exchange")), price, **kw)
            iv = q.get("interval", "1day")
            size = int(q.get("outputsize", "30"))
            if iv == "1day":
                rows = [(d.isoformat(), o, h, l, c, v) for d, o, h, l, c, v in ser]
            elif iv in ("5min", "15min", "30min", "1h"):
                m = {"5min": 5, "15min": 15, "30min": 30, "1h": 60}[iv]
                days = max(1, min(5, size * m // 390 + 1))
                rows = []
                for k in range(days - 1, -1, -1):
                    pc, lc = ser[-k - 2][4], ser[-k - 1][4]
                    bars = intraday(s + str(k) + iv, pc, lc, m, 0)
                    shift = timedelta(days=0)
                    # move the session back k weekdays
                    dd = 0
                    back = k
                    while back:
                        dd += 1
                        if (bars[0][0] - timedelta(days=dd)).weekday() < 5:
                            back -= 1
                    shift = timedelta(days=dd)
                    tz = timezone.utc if q.get("timezone") == "UTC" else timezone(timedelta(hours=-5))
                    rows += [((t - shift).astimezone(tz).strftime("%Y-%m-%d %H:%M:%S"),
                              o, h, l, c, v) for t, o, h, l, c, v in bars]
            else:
                self.send(200, {"code": 400, "message": "**interval** is incorrect", "status": "error"})
                return
            rows = rows[-size:][::-1]     # newest first, like the real one
            if "EMPTY" in s.upper():
                rows = []
            self.send(200, {
                "meta": {"symbol": s, "interval": iv, "currency": cur,
                         "exchange_timezone": "America/New_York", "exchange": "NYSE",
                         "mic_code": "XNYS", "type": "Common Stock"},
                "values": [{"datetime": d, "open": "%.5f" % o, "high": "%.5f" % h, "low": "%.5f" % l,
                            "close": "%.5f" % c, "volume": "%d" % v} for d, o, h, l, c, v in rows],
                "status": "ok"})
            return
        self.send(404, {"code": 404, "message": "endpoint", "status": "error"})

    def tw_sym(self, s, exchange):
        if exchange == "BCBA" or s.endswith(":BCBA"):
            return s.split(":")[0] + ".BA"
        return s

    def tw_quote(self, s):
        info = lookup(self.tw_sym(s, None))
        if not info:
            return {"code": 404, "message": "**symbol** not found: %s" % s, "status": "error"}
        name, cur, price, kw = info
        ser = daily_series(self.tw_sym(s, None), price, **kw)
        d, o, h, l, c, v = ser[-1]
        pc = ser[-2][4]
        yr = ser[-252:]
        lo52 = min(x[3] for x in yr)
        hi52 = max(x[2] for x in yr)
        return {"symbol": s, "name": name, "exchange": "NYSE", "mic_code": "XNYS", "currency": cur,
                "datetime": d.isoformat(), "timestamp": int(time.time()) - 900,
                "open": "%.5f" % o, "high": "%.5f" % h, "low": "%.5f" % l, "close": "%.5f" % c,
                "volume": "%d" % v, "previous_close": "%.5f" % pc, "change": "%.5f" % (c - pc),
                "percent_change": "%.5f" % ((c / pc - 1) * 100), "average_volume": "%d" % v,
                "is_market_open": True,
                "fifty_two_week": {"low": "%.5f" % lo52, "high": "%.5f" % hi52,
                                   "low_change": "%.5f" % (c - lo52), "high_change": "%.5f" % (c - hi52),
                                   "low_change_percent": "%.5f" % ((c / lo52 - 1) * 100),
                                   "high_change_percent": "%.5f" % ((c / hi52 - 1) * 100),
                                   "range": "%.6f - %.6f" % (lo52, hi52)}}

    # ---- Alpha Vantage ----
    def alpha(self, q):
        key = q.get("apikey", "")
        if not key or key == "bad":
            self.send(200, {"Error Message": "the parameter apikey is invalid or missing. Please claim your free API key on (https://www.alphavantage.co/support/#api-key). It should take less than 20 seconds."})
            return
        if over_limit("alpha"):
            self.send(200, {"Information": "We have detected your API key as %s and our standard API rate limit is 25 requests per day. Please subscribe to any of the premium plans at https://www.alphavantage.co/premium/ to instantly remove all daily rate limits." % key})
            return
        fn = q.get("function", "")
        if fn == "SYMBOL_SEARCH":
            kw = q.get("keywords", "").upper()
            best = []
            for k, (n, ex, cur, p) in COMPANIES.items():
                if kw and (kw in k or kw in n.upper()):
                    best.append({"1. symbol": k + (".BA" if ex == "BCBA" else ""), "2. name": n,
                                 "3. type": "Equity", "4. region": "Argentina" if ex == "BCBA" else "United States",
                                 "5. marketOpen": "09:30", "6. marketClose": "16:00",
                                 "7. timezone": "UTC-04", "8. currency": cur, "9. matchScore": "0.8000"})
            self.send(200, {"bestMatches": best})
            return
        s = q.get("symbol", "")
        if self.special(s):
            return
        info = lookup(s)
        if not info:
            self.send(200, {"Error Message": "Invalid API call. Please retry or visit the documentation (https://www.alphavantage.co/documentation/) for TIME_SERIES_DAILY."})
            return
        name, cur, price, kw = info
        ser = daily_series(s, price, **kw)
        if fn == "GLOBAL_QUOTE":
            d, o, h, l, c, v = ser[-1]
            pc = ser[-2][4]
            self.send(200, {"Global Quote": {
                "01. symbol": s, "02. open": "%.4f" % o, "03. high": "%.4f" % h, "04. low": "%.4f" % l,
                "05. price": "%.4f" % c, "06. volume": "%d" % v, "07. latest trading day": d.isoformat(),
                "08. previous close": "%.4f" % pc, "09. change": "%.4f" % (c - pc),
                "10. change percent": "%.4f%%" % ((c / pc - 1) * 100)}})
        elif fn in ("TIME_SERIES_DAILY", "TIME_SERIES_WEEKLY"):
            weekly = fn == "TIME_SERIES_WEEKLY"
            if weekly:
                wk = {}
                for d, o, h, l, c, v in ser:
                    key2 = d + timedelta(days=4 - d.weekday())     # the week's Friday
                    if key2 not in wk:
                        wk[key2] = [o, h, l, c, v]
                    else:
                        w = wk[key2]
                        w[1] = max(w[1], h); w[2] = min(w[2], l); w[3] = c; w[4] += v
                rows = [(k, *v) for k, v in sorted(wk.items())]
                rows[-1] = (ser[-1][0],) + rows[-1][1:]
            else:
                full = q.get("outputsize") == "full"
                if full:
                    self.send(200, {"Information": "Thank you for using Alpha Vantage! The outputsize=full parameter value is a premium feature for the TIME_SERIES_DAILY endpoint."})
                    return
                rows = ser[-100:]
            ts = {}
            for d, o, h, l, c, v in reversed(rows):
                ts[d.isoformat()] = {"1. open": "%.4f" % o, "2. high": "%.4f" % h, "3. low": "%.4f" % l,
                                     "4. close": "%.4f" % c, "5. volume": "%d" % v}
            self.send(200, {"Meta Data": {"1. Information": "Daily Prices (open, high, low, close) and Volumes",
                                          "2. Symbol": s, "3. Last Refreshed": ser[-1][0].isoformat(),
                                          "4. Output Size": "Compact", "5. Time Zone": "US/Eastern"},
                            ("Weekly Time Series" if weekly else "Time Series (Daily)"): ts})
        else:
            self.send(200, {"Error Message": "This API function (%s) does not exist." % fn})

    # ---- Finnhub ----
    def finnhub(self, parts, q):
        tok = q.get("token", "")
        if not tok or tok == "bad":
            self.send(401, {"error": "Invalid API key"})
            return
        if over_limit("finnhub"):
            self.send(429, {"error": "API limit reached. Please try again later. Remaining Limit: 0"})
            return
        ep = "/".join(parts)
        if ep == "api/v1/search":
            qq = q.get("q", "").upper()
            res = [{"description": n.upper(), "displaySymbol": k + (".BA" if ex == "BCBA" else ""),
                    "symbol": k + (".BA" if ex == "BCBA" else ""), "type": "Common Stock"}
                   for k, (n, ex, cur, p) in COMPANIES.items() if qq and (qq in k or qq in n.upper())]
            self.send(200, {"count": len(res), "result": res})
            return
        if ep == "api/v1/quote":
            s = q.get("symbol", "")
            if self.special(s):
                return
            info = lookup(s)
            if not info:
                # the real one answers zeros for a symbol it does not know
                self.send(200, {"c": 0, "d": None, "dp": None, "h": 0, "l": 0, "o": 0, "pc": 0, "t": 0})
                return
            name, cur, price, kw = info
            ser = daily_series(s, price, **kw)
            d, o, h, l, c, v = ser[-1]
            pc = ser[-2][4]
            self.send(200, {"c": f2(c), "d": f2(c - pc), "dp": f2((c / pc - 1) * 100), "h": f2(h),
                            "l": f2(l), "o": f2(o), "pc": f2(pc), "t": int(time.time()) - 900})
            return
        if ep == "api/v1/stock/candle":
            self.send(403, {"error": "You don't have access to this resource."})
            return
        self.send(404, {"error": "endpoint"})


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8248)
    ap.add_argument("--strict", action="store_true", help="enforce the real rate limits")
    ap.add_argument("--no-key-stooq", action="store_true", help="Stooq wants an apikey")
    ap.add_argument("--quiet", action="store_true")
    ARGS = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), H)
    print("fake_quotes on http://127.0.0.1:%d (strict=%s)" % (ARGS.port, ARGS.strict), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
