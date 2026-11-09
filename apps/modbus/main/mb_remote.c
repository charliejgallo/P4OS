/*
 * P4OS - Modbus: the portal's page drives the app (docs/PORTAL-PAGES.md,
 * "Live data"). No firmware of its own: the live channel both ways.
 *
 * The page sends lines of key=value (one or several per message):
 *
 *   perfil=<file> | perfil=          use a profile, or none
 *   abrir=1 | abrir=0                open or close the link
 *   rol=0|1|2                        master, slave, monitor
 *   puerto=uart.a  baud=9600  formato=8N1  espera=300  silencio=5
 *   tcp=host:port  tr=rtu|tcp
 *   pedido=unit,fc,addr,n[,v v v]    a request by hand, once (master)
 *   vivo=1|0  periodo=500            the same request over and over
 *   punto=<i>,<value>                write a profile point (or set it, as slave)
 *   falla=0..5  demora=ms  unidad=n  animar=1|0     the slave
 *   pasarela=1|0  pasarela_puerto=502   the TCP -> RTU gateway (master on RTU)
 *   borrar=cuentas | borrar=trafico
 *   herramienta=unidades | velocidad | parar
 *
 * and reads two keys while it looks:
 *
 *   estado   JSON: the link, the role, the counts of each role, the last
 *            request's answer, the tools, the slave's settings, the
 *            monitor's units (twice a second)
 *   trafico  JSON: the newest frames of the log, as the Traffic tab shows
 *            them (when there are new ones)
 *
 * The values a profile reads go on as "valores" (mb_ui_prof.c). Everything
 * here runs from the app's timer, LVGL's thread, like a tap would.
 */
#include "mb_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_APP "aos.modbus"
#define TRAFFIC_N 80

/* ---- a small JSON writer into a fixed buffer ---- */

typedef struct { char *b; size_t n, k; } jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    size_t l = strlen(s);
    if (w->k + l + 1 >= w->n) return;
    memcpy(w->b + w->k, s, l);
    w->k += l;
    w->b[w->k] = 0;
}

static void jw_fmt(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    if (w->k + 2 >= w->n) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(w->b + w->k, w->n - w->k, fmt, ap);
    va_end(ap);
    if (r > 0) w->k = w->k + (size_t)r < w->n ? w->k + (size_t)r : w->n - 1;
}

static void jw_str(jw_t *w, const char *s)
{
    if (w->k + 3 >= w->n) return;
    w->b[w->k++] = '"';
    for (; *s && w->k + 8 < w->n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { w->b[w->k++] = '\\'; w->b[w->k++] = (char)c; }
        else if (c < 0x20) w->k += (size_t)snprintf(w->b + w->k, w->n - w->k, "\\u%04x", c);
        else w->b[w->k++] = (char)c;
    }
    w->b[w->k++] = '"';
    w->b[w->k] = 0;
}

/* ---- the orders ---- */

static bool flag(const char *v) { return v[0] == '1' || !strcmp(v, "on") || !strcmp(v, "si"); }

static void order(char *key, char *v)
{
    if (!strcmp(key, "perfil")) { prof_use(v); return; }
    mb_lock();
    if (!strcmp(key, "abrir")) {
        MB.want_open = flag(v);
        if (!MB.want_open) { MB.m_live = false; MB.tool_stop = true; }
        MB.err[0] = 0;
    } else if (!strcmp(key, "rol")) {
        int r = atoi(v);
        if (r >= 0 && r < MB_ROLE_COUNT && MB.role != (mb_role_t)r) {
            MB.role = (mb_role_t)r;
            MB.m_live = false;
            MB.tool_stop = true;
            MBU.tab = r;                /* the screen follows */
        }
    } else if (!strcmp(key, "puerto")) snprintf(MB.cfg.port, sizeof MB.cfg.port, "%.15s", v);
    else if (!strcmp(key, "baud")) { long b = atol(v); if (b >= 300 && b <= 1000000) MB.cfg.baud = (uint32_t)b; }
    else if (!strcmp(key, "formato")) {
        if (strlen(v) == 3 && v[0] == '8' && (v[1] == 'N' || v[1] == 'E' || v[1] == 'O') && (v[2] == '1' || v[2] == '2')) {
            MB.cfg.par = v[1];
            MB.cfg.stop = (uint8_t)(v[2] - '0');
        }
    } else if (!strcmp(key, "tr")) MB.cfg.tr = !strcmp(v, "tcp") ? MB_TR_TCP : MB_TR_RTU;
    else if (!strcmp(key, "tcp")) {
        char *c = strrchr(v, ':');
        int port = 502;
        if (c) { *c = 0; port = atoi(c + 1) > 0 ? atoi(c + 1) : 502; }
        snprintf(MB.cfg.host, sizeof MB.cfg.host, "%s", v);
        MB.cfg.tport = (uint16_t)port;
    } else if (!strcmp(key, "espera")) { int t = atoi(v); if (t >= 20 && t <= 10000) MB.cfg.timeout_ms = (uint16_t)t; }
    else if (!strcmp(key, "silencio")) { int t = atoi(v); if (t >= 0 && t <= 1000) MB.cfg.gap_ms = (uint16_t)t; }
    else if (!strcmp(key, "pedido")) {
        /* unit,fc,addr,n[,values separated by spaces] */
        mb_mreq_t q = MB.mreq;
        char *p = v, *end;
        long f[4];
        int k = 0;
        for (; k < 4; k++) {
            f[k] = strtol(p, &end, 0);
            if (end == p) break;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        if (k >= 3) {
            q.unit = (uint8_t)(f[0] < 0 ? 0 : f[0] > 247 ? 247 : f[0]);
            q.fc = (uint8_t)f[1];
            q.addr = (uint16_t)f[2];
            bool one = q.fc == 5 || q.fc == 6 || q.fc == 8;     /* the fourth number is the value */
            if (k == 4 && !one) q.n = (uint16_t)(f[3] < 1 ? 1 : f[3] > 2000 ? 2000 : f[3]);
            int wn = 0;
            while (*p && wn < 123) {
                long x = strtol(p, &end, 0);
                if (end == p) break;
                q.wv[wn++] = (uint16_t)x;
                p = end;
                while (*p == ',' || *p == ' ') p++;
            }
            if (one && k == 4) { q.wv[0] = (uint16_t)f[3]; q.wn = 1; }    /* pedido=1,6,10,1234 */
            else if (wn) q.wn = (uint16_t)wn;
            MB.mreq = q;
            if (MB.role == MB_ROLE_MASTER && !MB.prof_on) {
                MB.want_open = true;
                MB.m_once = true;
            }
        }
    } else if (!strcmp(key, "vivo")) {
        MB.m_live = flag(v) && MB.mreq.fc <= 4;
        if (MB.m_live) MB.want_open = true;
    } else if (!strcmp(key, "periodo")) { int t = atoi(v); if (t >= 50 && t <= 60000) MB.m_period_ms = (uint16_t)t; }
    else if (!strcmp(key, "falla")) { int f = atoi(v); if (f >= 0 && f < MB_FAULT_COUNT) MB.s_fault = f; }
    else if (!strcmp(key, "demora")) { int t = atoi(v); if (t >= 0 && t <= 5000) MB.s_delay_ms = (uint16_t)t; }
    else if (!strcmp(key, "unidad")) { int u = atoi(v); if (u >= 1 && u <= 247) MB.s_unit = (uint8_t)u; }
    else if (!strcmp(key, "animar")) MB.s_anim = flag(v);
    else if (!strcmp(key, "pasarela")) { MB.gw_on = flag(v); MB.tcp_err[0] = 0; if (MB.gw_on) MB.want_open = true; }
    else if (!strcmp(key, "pasarela_puerto")) { long p = atol(v); if (p > 0 && p < 65536) MB.gw_port = (uint16_t)p; }
    else if (!strcmp(key, "herramienta")) {
        bool rtu = MB.cfg.tr == MB_TR_RTU;
        if (!strcmp(v, "parar")) MB.tool_stop = true;
        else if (rtu && MB.tool == MB_TOOL_NONE && MB.role == MB_ROLE_MASTER) {
            MB.tool = !strcmp(v, "velocidad") ? MB_TOOL_BAUD : MB_TOOL_SCAN;
            MB.tool_stop = false;
            MB.tool_pos = 0;
            MB.m_live = false;
            MB.want_open = true;
        }
    } else if (!strcmp(key, "punto")) {
        char *c = strchr(v, ',');
        if (c) {
            int i = atoi(v);
            double x = strtod(c + 1, NULL);
            mb_unlock();
            prof_point_write(i, x);
            mb_lock();
        }
    } else if (!strcmp(key, "borrar")) {
        mb_unlock();
        if (!strcmp(v, "trafico")) mb_bus_clear_log();
        else mb_bus_clear_stats();
        mb_lock();
    }
    MB.seq++;
    mb_unlock();
}

static void take_orders(void)
{
    char msg[1100];
    int n, guard = 0;
    bool any = false;
    while (guard++ < 16 && (n = aos_hal_live_take(LIVE_APP, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        /* strtok is not among the firmware's exports */
        for (char *line = msg, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            order(line, eq + 1);
            any = true;
        }
    }
    if (!any) return;
    mb_settings_save();
    if (!MBU.overlay) mb_rebuild_page();
    mb_bar_refresh();
}

/* ---- the state ---- */

static const char *const ROLE_ID[MB_ROLE_COUNT] = { "maestro", "esclavo", "monitor" };

static void put_state(void)
{
    static char buf[16384];
    static mb_shared_t s;                   /* a copy taken under the lock (PSRAM, .bss) */
    jw_t w = { buf, sizeof buf, 0 };
    mb_lock();
    /* only what is shown: the log and the tables stay where they are */
    s.cfg = MB.cfg;
    s.role = MB.role;
    s.want_open = MB.want_open;
    s.open = MB.open;
    memcpy(s.err, MB.err, sizeof s.err);
    memcpy(s.desc, MB.desc, sizeof s.desc);
    s.mreq = MB.mreq;
    s.m_live = MB.m_live;
    s.m_period_ms = MB.m_period_ms;
    s.m_rc = MB.m_rc;
    s.m_got_fc = MB.m_got_fc;
    s.m_got_addr = MB.m_got_addr;
    s.m_got_n = MB.m_got_n;
    memcpy(s.m_val, MB.m_val, sizeof s.m_val);
    memcpy(s.m_text, MB.m_text, sizeof s.m_text);
    s.m_seq = MB.m_seq;
    s.m_req = MB.m_req; s.m_ans = MB.m_ans; s.m_tmo = MB.m_tmo; s.m_crc = MB.m_crc; s.m_exc = MB.m_exc;
    s.m_rt_last = MB.m_rt_last; s.m_rt_sum = MB.m_rt_sum; s.m_rt_max = MB.m_rt_max;
    s.tool = MB.tool; s.tool_pos = MB.tool_pos; s.tool_total = MB.tool_total;
    memcpy(s.found, MB.found, sizeof s.found);
    memcpy(s.bauds_found, MB.bauds_found, sizeof s.bauds_found);
    s.nbauds_found = MB.nbauds_found;
    s.s_unit = MB.s_unit; s.s_fault = MB.s_fault; s.s_delay_ms = MB.s_delay_ms; s.s_anim = MB.s_anim;
    s.s_req = MB.s_req; s.s_mine = MB.s_mine; s.s_other = MB.s_other; s.s_bcast = MB.s_bcast; s.s_crc = MB.s_crc; s.s_exc = MB.s_exc;
    memcpy(s.s_last, MB.s_last, sizeof s.s_last);
    memcpy(s.units, MB.units, sizeof s.units);
    s.nunits = MB.nunits;
    s.mon_frames = MB.mon_frames; s.mon_bytes = MB.mon_bytes; s.mon_crc = MB.mon_crc; s.mon_noise = MB.mon_noise;
    s.mon_parity = MB.mon_parity; s.mon_frame = MB.mon_frame;
    s.gw_on = MB.gw_on; s.gw_port = MB.gw_port; s.gw_req = MB.gw_req; s.gw_ok = MB.gw_ok; s.gw_fail = MB.gw_fail;
    memcpy(s.tcp_peer, MB.tcp_peer, sizeof s.tcp_peer);
    memcpy(s.tcp_err, MB.tcp_err, sizeof s.tcp_err);
    s.nbauds_clean = MB.nbauds_clean;
    s.prof_on = MB.prof_on;
    memcpy(s.prof.file, MB.prof.file, sizeof s.prof.file);
    memcpy(s.prof.name, MB.prof.name, sizeof s.prof.name);
    mb_unlock();

    jw_raw(&w, "{\"rol\":");
    jw_str(&w, ROLE_ID[s.role]);
    jw_fmt(&w, ",\"quiere\":%s,\"abierto\":%s,\"error\":", s.want_open ? "true" : "false", s.open ? "true" : "false");
    jw_str(&w, s.err);
    jw_raw(&w, ",\"desc\":");
    jw_str(&w, s.desc);
    jw_raw(&w, ",\"conexion\":{\"tr\":");
    jw_str(&w, s.cfg.tr == MB_TR_TCP ? "tcp" : "rtu");
    jw_raw(&w, ",\"puerto\":");
    jw_str(&w, s.cfg.port);
    jw_fmt(&w, ",\"baud\":%lu,\"formato\":\"8%c%u\",\"host\":", (unsigned long)s.cfg.baud, s.cfg.par ? s.cfg.par : 'N', s.cfg.stop ? s.cfg.stop : 1);
    jw_str(&w, s.cfg.host);
    jw_fmt(&w, ",\"tport\":%u,\"espera\":%u,\"silencio\":%u}", s.cfg.tport, s.cfg.timeout_ms, s.cfg.gap_ms);
    jw_raw(&w, ",\"perfil\":");
    jw_str(&w, s.prof_on ? s.prof.file : "");
    jw_raw(&w, ",\"perfil_nombre\":");
    jw_str(&w, s.prof_on ? s.prof.name : "");

    /* master */
    jw_fmt(&w, ",\"maestro\":{\"pedido\":{\"unidad\":%u,\"fc\":%u,\"desde\":%u,\"cantidad\":%u,\"valores\":[",
           s.mreq.unit, s.mreq.fc, s.mreq.addr, s.mreq.n);
    for (int i = 0; i < s.mreq.wn && i < 123; i++) jw_fmt(&w, "%s%u", i ? "," : "", s.mreq.wv[i]);
    jw_fmt(&w, "]},\"vivo\":%s,\"periodo\":%u,\"seq\":%lu,\"rc\":%d,\"resultado\":", s.m_live ? "true" : "false", s.m_period_ms,
           (unsigned long)s.m_seq, s.m_rc);
    jw_str(&w, s.m_seq ? mb_strerror(s.m_rc) : "");
    jw_fmt(&w, ",\"fc\":%u,\"desde\":%u,\"n\":%u,\"vals\":[", s.m_got_fc, s.m_got_addr, s.m_got_n);
    int nv = s.m_got_n < 256 ? s.m_got_n : 256;
    for (int i = 0; i < nv && !s.m_rc; i++) jw_fmt(&w, "%s%u", i ? "," : "", s.m_val[i]);
    jw_raw(&w, "],\"texto\":");
    jw_str(&w, s.m_text);
    jw_fmt(&w, ",\"cuentas\":{\"pedidos\":%lu,\"respuestas\":%lu,\"sin_respuesta\":%lu,\"crc\":%lu,\"excepciones\":%lu,"
               "\"ultima_ms\":%lu,\"promedio_ms\":%lu,\"maxima_ms\":%lu}",
           (unsigned long)s.m_req, (unsigned long)s.m_ans, (unsigned long)s.m_tmo, (unsigned long)s.m_crc, (unsigned long)s.m_exc,
           (unsigned long)s.m_rt_last, (unsigned long)(s.m_ans ? s.m_rt_sum / s.m_ans : 0), (unsigned long)s.m_rt_max);
    jw_fmt(&w, ",\"herramienta\":{\"activa\":%d,\"pos\":%d,\"total\":%d,\"unidades\":[", s.tool, s.tool_pos, s.tool_total);
    bool first = true;
    for (int u = 1; u <= 247; u++) {
        if (!s.found[u]) continue;
        jw_fmt(&w, "%s[%d,%u]", first ? "" : ",", u, s.found[u]);
        first = false;
    }
    jw_fmt(&w, "],\"limpias\":%d,\"velocidades\":[", s.nbauds_clean);
    for (int i = 0; i < s.nbauds_found && i < 8; i++) { if (i) jw_raw(&w, ","); jw_str(&w, s.bauds_found[i]); }
    jw_raw(&w, "]}}");

    /* the gateway, and the client of the TCP server (gateway or TCP slave) */
    jw_fmt(&w, ",\"pasarela\":{\"on\":%s,\"puerto\":%u,\"pedidos\":%lu,\"ok\":%lu,\"fallas\":%lu,\"cliente\":",
           s.gw_on ? "true" : "false", s.gw_port ? s.gw_port : 502, (unsigned long)s.gw_req, (unsigned long)s.gw_ok, (unsigned long)s.gw_fail);
    jw_str(&w, s.tcp_peer);
    jw_raw(&w, ",\"error\":");
    jw_str(&w, s.tcp_err);
    jw_raw(&w, "}");

    /* slave */
    jw_fmt(&w, ",\"esclavo\":{\"unidad\":%u,\"falla\":%d,\"demora\":%u,\"animar\":%s,\"cuentas\":{\"oidas\":%lu,\"propias\":%lu,"
               "\"otras\":%lu,\"broadcast\":%lu,\"crc\":%lu,\"excepciones\":%lu},\"ultimo\":",
           s.s_unit, s.s_fault, s.s_delay_ms, s.s_anim ? "true" : "false", (unsigned long)s.s_req, (unsigned long)s.s_mine,
           (unsigned long)s.s_other, (unsigned long)s.s_bcast, (unsigned long)s.s_crc, (unsigned long)s.s_exc);
    jw_str(&w, s.s_last);
    jw_raw(&w, "}");

    /* monitor */
    jw_fmt(&w, ",\"monitor\":{\"tramas\":%lu,\"bytes\":%lu,\"crc\":%lu,\"ruido\":%lu,\"paridad\":%lu,\"parada\":%lu,\"unidades\":[",
           (unsigned long)s.mon_frames, (unsigned long)s.mon_bytes, (unsigned long)s.mon_crc, (unsigned long)s.mon_noise,
           (unsigned long)s.mon_parity, (unsigned long)s.mon_frame);
    for (int i = 0; i < s.nunits; i++) {
        const mb_unit_t *u = &s.units[i];
        char last[80];
        uint8_t fake[6] = { u->unit, u->last_fc, (uint8_t)(u->last_addr >> 8), (uint8_t)u->last_addr, (uint8_t)(u->last_n >> 8), (uint8_t)u->last_n };
        mb_describe(fake, 6, true, last, sizeof last);
        jw_fmt(&w, "%s{\"unidad\":%u,\"pedidos\":%lu,\"respuestas\":%lu,\"sin_respuesta\":%lu,\"excepciones\":%lu,\"crc\":%lu,"
                   "\"promedio_ms\":%lu,\"maxima_ms\":%lu,\"ultimo\":",
               i ? "," : "", u->unit, (unsigned long)u->req, (unsigned long)u->rsp, (unsigned long)u->timeouts, (unsigned long)u->exc,
               (unsigned long)u->crc, (unsigned long)(u->rt_n ? u->rt_sum / u->rt_n : 0), (unsigned long)u->rt_max);
        jw_str(&w, last);
        jw_raw(&w, "}");
    }
    jw_raw(&w, "]}}");
    aos_hal_live_put(LIVE_APP, "estado", "application/json", buf, w.k);
}

static void put_traffic(void)
{
    static uint32_t seen = 0xFFFFFFFF;
    mb_lock();
    uint32_t seq = MB.log_seq;
    uint64_t t0 = MB.t0_us;
    mb_unlock();
    if (seq == seen) return;
    seen = seq;
    static char buf[40000];
    static mb_log_t e;
    jw_t w = { buf, sizeof buf, 0 };
    jw_fmt(&w, "{\"seq\":%lu,\"tramas\":[", (unsigned long)seq);
    int row = 0;
    for (uint32_t k = seq; k-- > 0 && row < TRAFFIC_N && seq - k <= MB_LOGN;) {
        if (!mb_log_get(k, &e)) break;
        char hex[3 * 64 + 8] = "", what[128] = "";
        size_t hk = 0;
        for (int i = 0; i < e.len && i < 64; i++) hk += (size_t)snprintf(hex + hk, sizeof hex - hk, "%s%02X", i ? " " : "", e.d[i]);
        bool tcp = e.fl & MB_LF_TCP;
        if (e.fl & MB_LF_NOTE) {
            snprintf(what, sizeof what, "%s", mb_note_text(e.note));
            if (e.note == MB_NOTE_TIMEOUT && e.len >= 2) snprintf(what, sizeof what, "%s: U%u %s", mb_note_text(e.note), e.d[0], mb_fc_name(e.d[1]));
            hex[0] = 0;
        } else if (e.fl & MB_LF_NOISE) snprintf(what, sizeof what, "%s", _("ruido: bytes que no forman una trama"));
        else if (tcp && e.len > 7) {
            uint8_t f[MB_MAXF];
            f[0] = e.d[6];
            memcpy(f + 1, e.d + 7, (size_t)(e.len - 7));
            mb_describe(f, e.len - 6, e.fl & MB_LF_REQ, what, sizeof what);
        } else mb_describe(e.d, e.len, (e.fl & MB_LF_REQ) != 0, what, sizeof what);
        if (!(e.fl & MB_LF_NOTE) && e.note) { size_t l = strlen(what); snprintf(what + l, sizeof what - l, " (%s)", mb_note_text(e.note)); }
        uint64_t dt = e.t_us > t0 ? e.t_us - t0 : 0;
        jw_fmt(&w, "%s{\"t\":%lu,\"fl\":%u,\"nota\":%u,\"silencio_us\":%lu,\"rt\":%u,\"hex\":", row ? "," : "",
               (unsigned long)(dt / 1000), e.fl, e.note, (unsigned long)e.gap_us, e.rt_ms);
        jw_str(&w, hex);
        jw_raw(&w, ",\"que\":");
        jw_str(&w, what);
        jw_raw(&w, "}");
        row++;
    }
    jw_raw(&w, "]}");
    aos_hal_live_put(LIVE_APP, "trafico", "application/json", buf, w.k);
}

void remote_tick(void)
{
    static uint32_t n;
    n++;
    take_orders();
    if (aos_hal_live_idle_ms(LIVE_APP) > 4000) return;     /* nobody looks */
    if (n % 3 == 0) put_state();
    if (n % 5 == 0) put_traffic();
}
