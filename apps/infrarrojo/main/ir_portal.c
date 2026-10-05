/*
 * P4OS - Infrarrojo: what the portal's page asks for.
 *
 * The page (web/infrarrojo.js) and the app meet on the card, as the other
 * apps' pages do; there is no JSON API of the firmware's in between. The
 * page writes a request and the app answers in a state file:
 *
 *   /data/ir_pedido.json   {"id": 7, "op": "send", "file": "tele.json", "button": 2}
 *                          {"id": 8, "op": "raw", "freq": 38000, "raw": [9000, 4500, ...]}
 *                          {"id": 9, "op": "code", "proto": "NEC", "addr": 4, "cmd": 8}
 *                          {"id": 10, "op": "ac", "file": "aire.json"}   its state, as saved
 *                          {"id": 11, "op": "learn"}   listen for 30 s more
 *                          {"id": 12, "op": "stop"}
 *   /data/ir_estado.json   {"id": 7, "ok": true, "msg": "...", "listening": false,
 *                           "rx": 28, "tx": 32, "captures": [ newest first, up to 6:
 *                             {"seq": 3, "proto": "NEC", "addr": 4, "cmd": 8, "text": "...",
 *                              "desc": "...", "freq": 38000, "raw": [...]} ]}
 *
 * The page writes through the portal's upload, which goes to a .part and is
 * renamed at the end, so a request is never read half written. The app
 * deletes it once read; the page knows it was done by the id in the state.
 * All of it works only while the app is alive (the page opens it).
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define KEEP      6
#define LISTEN_MS 30000

typedef struct {
    ir_code_t code;
    bool      known;
    uint32_t  seq;
    uint32_t *d;
    int       n;
} pcap_t;

static struct {
    bool     listening;
    uint32_t listen_until, seen, last_id, check_ms;
    pcap_t   cap[KEEP];
    int      ncap;
    ir_capture_t *tmp;
    char     msg[96];
    bool     ok, dirty;
} P;

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", aos_hal_path_data(), name);
}

static void write_state(void)
{
    ir_sb_t sb = { 0 };
    int rx, tx;
    ir_hw_pins(&rx, &tx);
    ir_sb_printf(&sb, "{\"id\": %u, \"ok\": %s, \"msg\": ", (unsigned)P.last_id, P.ok ? "true" : "false");
    ir_sb_json(&sb, P.msg);
    ir_sb_printf(&sb, ", \"listening\": %s, \"rx\": %d, \"tx\": %d, \"sent\": %u, \"captures\": [",
                 P.listening ? "true" : "false", rx, tx, (unsigned)ir_hw_sent());
    for (int i = 0; i < P.ncap; i++) {
        pcap_t *c = &P.cap[i];
        char t[200];
        ir_sb_printf(&sb, "%s\n  {\"seq\": %u", i ? "," : "", (unsigned)c->seq);
        if (c->known) {
            ir_sb_printf(&sb, ", \"proto\": \"%s\", \"addr\": %u, \"cmd\": %u", c->code.proto,
                         (unsigned)c->code.addr, (unsigned)c->code.cmd);
            if (!strcmp(c->code.proto, "Kaseikyo")) ir_sb_printf(&sb, ", \"vendor\": %u", (unsigned)c->code.extra);
            ir_code_text(&c->code, t, sizeof t);
        } else {
            snprintf(t, sizeof t, "%s", _("crudo"));
        }
        ir_sb_put(&sb, ", \"text\": ", -1);
        ir_sb_json(&sb, t);
        ir_describe(c->d, c->n, t, sizeof t);
        ir_sb_put(&sb, ", \"desc\": ", -1);
        ir_sb_json(&sb, t);
        const ir_proto_t *p = c->known ? ir_proto_find(c->code.proto) : NULL;
        ir_sb_printf(&sb, ", \"freq\": %u, \"raw\": [", (unsigned)(p ? p->carrier : 38000));
        for (int k = 0; k < c->n; k++) ir_sb_printf(&sb, k ? ",%u" : "%u", (unsigned)c->d[k]);
        ir_sb_put(&sb, "]}", 2);
    }
    ir_sb_put(&sb, "\n]}\n", -1);
    char path[128], tmp[136];
    mkdir(aos_hal_path_data(), 0777);
    path_of("ir_estado.json", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        bool ok = sb.s && fwrite(sb.s, 1, (size_t)sb.n, f) == (size_t)sb.n;
        ok = fclose(f) == 0 && ok;
        if (ok) { remove(path); rename(tmp, path); }
        else remove(tmp);
    }
    ir_sb_free(&sb);
    P.dirty = false;
}

static void say(bool ok, const char *msg)
{
    P.ok = ok;
    ir_copy(P.msg, sizeof P.msg, msg);
    P.dirty = true;
}

static void do_request(const ij_doc_t *j)
{
    P.last_id = (uint32_t)ij_num(j, ij_get(j, 0, "id"), 0);
    const char *op = ij_str(j, ij_get(j, 0, "op"), "");
    if (!strcmp(op, "learn")) {
        if (!P.listening) P.seen = ir_hw_capture_seq();
        P.listening = true;
        P.listen_until = (uint32_t)aos_hal_uptime_ms() + LISTEN_MS;
        ir_hw_listen(IR_WHO_PORTAL, true);
        say(true, _("Escuchando"));
    } else if (!strcmp(op, "stop")) {
        P.listening = false;
        ir_hw_listen(IR_WHO_PORTAL, false);
        say(true, _("Listo"));
    } else if (!strcmp(op, "send") || !strcmp(op, "ac")) {
        const char *file = ij_str(j, ij_get(j, 0, "file"), "");
        ir_dev_t d;
        if (!file[0] || strchr(file, '/') || !ir_dev_load(file, &d)) { say(false, _("No encuentro ese aparato")); return; }
        bool ok;
        if (!strcmp(op, "ac")) {
            ok = ir_ac_send(&d);
        } else {
            int b = (int)ij_num(j, ij_get(j, 0, "button"), -1);
            ok = b >= 0 && b < d.nbtn && ir_hw_send_button(&d.btn[b], ij_num(j, ij_get(j, 0, "repeat"), 0) != 0);
        }
        ir_dev_clear(&d);
        say(ok, ok ? _("Mandado") : _("No se pudo mandar"));
    } else if (!strcmp(op, "raw") || !strcmp(op, "code")) {
        ir_button_t b;
        memset(&b, 0, sizeof b);
        if (!strcmp(op, "code")) {
            const ir_proto_t *p = ir_proto_find(ij_str(j, ij_get(j, 0, "proto"), ""));
            if (!p) { say(false, _("Protocolo desconocido")); return; }
            ir_copy(b.code.proto, sizeof b.code.proto, p->name);
            b.code.addr = (uint32_t)ij_num(j, ij_get(j, 0, "addr"), 0) & p->addr_max;
            b.code.cmd = (uint32_t)ij_num(j, ij_get(j, 0, "cmd"), 0) & p->cmd_max;
            b.code.extra = (uint32_t)ij_num(j, ij_get(j, 0, "vendor"), 0x2002);
        } else {
            int raw = ij_get(j, 0, "raw");
            int n = raw >= 0 ? j->n[raw].count : 0;
            if (n <= 0 || n > IR_TX_MAX) { say(false, _("Sin duraciones")); return; }
            uint32_t *d = ir_alloc((size_t)n * sizeof *d);
            if (!d) return;
            int k = 0;
            IJ_EACH(j, raw, c) {
                double v = ij_num(j, c, 0);
                if (v < 0) v = -v;
                if (v >= 1) d[k++] = (uint32_t)v;
            }
            ir_button_set_raw(&b, d, k, (uint32_t)ij_num(j, ij_get(j, 0, "freq"), 38000));
            ir_free(d);
        }
        bool ok = ir_hw_send_button(&b, false);
        ir_button_clear(&b);
        say(ok, ok ? _("Mandado") : _("No se pudo mandar"));
    } else if (!strcmp(op, "reload")) {
        ir_remote_reload();
        say(true, _("Listo"));
    } else {
        say(false, _("Pedido desconocido"));
    }
}

static void take(const ir_capture_t *c)
{
    ir_code_t code;
    bool known = ir_decode(c->d, c->n, &code);
    if (known && code.repeat) return;
    int n = known ? ir_frame_len(c->d, c->n) : c->n;
    ir_free(P.cap[KEEP - 1].d);
    memmove(&P.cap[1], &P.cap[0], (KEEP - 1) * sizeof P.cap[0]);
    pcap_t *p = &P.cap[0];
    memset(p, 0, sizeof *p);
    p->d = ir_alloc((size_t)n * sizeof *p->d);
    if (!p->d) return;
    memcpy(p->d, c->d, (size_t)n * sizeof *p->d);
    p->n = n;
    p->code = code;
    p->known = known;
    p->seq = c->seq;
    if (P.ncap < KEEP) P.ncap++;
    P.dirty = true;
}

void ir_portal_tick(void)
{
    static bool told;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!told) { told = true; P.ok = true; P.dirty = true; }     /* the page sees the app is up */
    if (now - P.check_ms >= 400) {
        P.check_ms = now;
        char path[128];
        path_of("ir_pedido.json", path, sizeof path);
        struct stat st;
        if (stat(path, &st) == 0) {
            FILE *f = fopen(path, "rb");
            size_t len = st.st_size > 0 && st.st_size < 256 * 1024 ? (size_t)st.st_size : 0;
            char *text = len ? ir_alloc(len + 1) : NULL;
            bool got = f && text && fread(text, 1, len, f) == len;
            if (f) fclose(f);
            remove(path);
            ij_doc_t j;
            if (got && ij_parse(&j, text, len)) {
                do_request(&j);
                ij_free(&j);
            } else {
                say(false, _("Pedido ilegible"));
            }
            ir_free(text);
        }
    }
    if (P.listening) {
        ir_hw_listen(IR_WHO_PORTAL, true);     /* again: the worker may be new */
        if (!P.tmp) P.tmp = ir_alloc(sizeof *P.tmp);
        if (P.tmp && ir_hw_capture(&P.seen, P.tmp)) take(P.tmp);
        if ((int32_t)(now - P.listen_until) >= 0) {
            P.listening = false;
            ir_hw_listen(IR_WHO_PORTAL, false);
            P.dirty = true;
        }
    }
    if (P.dirty) write_state();
}
