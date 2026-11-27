/*
 * Malla - what is kept on the card, and the link that is remembered.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * One file per node the board talked to, <data>/malla/<!id>.bin, so a
 * second node brings its own history and does not mix with the first:
 *
 *   "MSH1" u32 version, u32 my_num, u32 sizeof node, sizeof msg, sizeof tele
 *   u32 n_nodes, n_msgs, n_tele, then the three arrays as they are in memory
 *   (the messages oldest first).
 *
 * A file whose sizes are not this build's is ignored: the node sends its
 * database again at every connect anyway, and only the messages and the
 * telemetry over time would be lost. What depends on this run's uptime
 * (when a node was heard, a message's clock when it had no date) is cleared
 * on loading. The save copies the model under the lock and writes outside
 * it, in a .tmp renamed at the end.
 *
 * The link is a preference, "malla_link": "tcp|host|port",
 * "ser|port|baud" or "ble|aa:bb:cc:dd:ee:ff|type|name".
 */
#include "msh.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STORE_VERSION 1

typedef struct {
    char     magic[4];
    uint32_t version, my_num, sz_node, sz_msg, sz_tele;
    uint32_t n_nodes, n_msgs, n_tele;
} store_hdr_t;

static void file_path(uint32_t num, char *out, size_t cap)
{
    snprintf(out, cap, "%s/malla", aos_hal_path_data());
    mkdir(out, 0777);
    snprintf(out, cap, "%s/malla/%08lx.bin", aos_hal_path_data(), (unsigned long)num);
}

void msh_store_load(uint32_t my_num)
{
    char path[200];
    file_path(my_num, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    store_hdr_t h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "MSH1", 4) || h.version != STORE_VERSION
        || h.my_num != my_num || h.sz_node != sizeof(msh_node_t) || h.sz_msg != sizeof(msh_msg_t)
        || h.sz_tele != sizeof(msh_tele_t) || h.n_nodes > MSH_NODES_MAX || h.n_msgs > MSH_MSGS_MAX
        || h.n_tele > MSH_TELE_NODES) {
        fclose(f);
        aos_hal_log(MSH_TAG, "%s: not this build's, ignored", path);
        return;
    }
    bool ok = fread(MSH.nodes, sizeof(msh_node_t), h.n_nodes, f) == h.n_nodes
           && fread(MSH.msgs, sizeof(msh_msg_t), h.n_msgs, f) == h.n_msgs
           && fread(MSH.tele, sizeof(msh_tele_t), h.n_tele, f) == h.n_tele;
    fclose(f);
    if (!ok) {
        MSH.n_nodes = MSH.msg_n = MSH.n_tele = 0;
        aos_hal_log(MSH_TAG, "%s: cut short, ignored", path);
        return;
    }
    MSH.n_nodes = (int)h.n_nodes;
    MSH.msg_n = (int)h.n_msgs;
    MSH.msg_head = 0;
    MSH.n_tele = (int)h.n_tele;
    for (int i = 0; i < MSH.n_nodes; i++) {
        MSH.nodes[i].heard_up = 0;
        MSH.nodes[i].tele = -1;
    }
    for (int i = 0; i < MSH.msg_n; i++) {
        MSH.msgs[i].up = 0;
        /* what was still on its way when the app closed will not be heard of again */
        if (MSH.msgs[i].state == MSH_MSG_QUEUED) MSH.msgs[i].state = MSH_MSG_FAILED;
    }
    for (int i = 0; i < MSH.n_tele; i++)
        for (int k = 0; k < MSH_TELE_N; k++) MSH.tele[i].s[k].up = 0;
    MSH.seq_nodes++;
    MSH.seq_msgs++;
    aos_hal_log(MSH_TAG, "loaded %d nodes, %d messages", MSH.n_nodes, MSH.msg_n);
}

/* The copy is made under the lock; the writing happens after. */
typedef struct {
    store_hdr_t h;
    uint8_t *blob;
    size_t len;
} snap_t;

static bool snapshot(snap_t *s)
{
    memset(s, 0, sizeof *s);
    if (!MSH.dirty || !MSH.my_num) return false;
    size_t nb = sizeof(msh_node_t) * (size_t)MSH.n_nodes;
    size_t mb = sizeof(msh_msg_t) * (size_t)MSH.msg_n;
    size_t tb = sizeof(msh_tele_t) * (size_t)MSH.n_tele;
    s->blob = (uint8_t *)msh_alloc(nb + mb + tb);
    if (!s->blob) return false;
    memcpy(s->h.magic, "MSH1", 4);
    s->h.version = STORE_VERSION;
    s->h.my_num = MSH.my_num;
    s->h.sz_node = sizeof(msh_node_t);
    s->h.sz_msg = sizeof(msh_msg_t);
    s->h.sz_tele = sizeof(msh_tele_t);
    s->h.n_nodes = (uint32_t)MSH.n_nodes;
    s->h.n_msgs = (uint32_t)MSH.msg_n;
    s->h.n_tele = (uint32_t)MSH.n_tele;
    memcpy(s->blob, MSH.nodes, nb);
    for (int i = 0; i < MSH.msg_n; i++) memcpy(s->blob + nb + sizeof(msh_msg_t) * (size_t)i, msh_msg_at(i), sizeof(msh_msg_t));
    memcpy(s->blob + nb + mb, MSH.tele, tb);
    s->len = nb + mb + tb;
    MSH.dirty = false;
    return true;
}

static void write_snap(snap_t *s)
{
    char path[200], tmp[210];
    file_path(s->h.my_num, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(&s->h, sizeof s->h, 1, f) == 1 && fwrite(s->blob, 1, s->len, f) == s->len;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) aos_hal_log(MSH_TAG, "could not save %s", path);
    free(s->blob);
}

void msh_store_save_locked(void)
{
    snap_t s;
    if (snapshot(&s)) write_snap(&s);
}

void msh_store_save(void)
{
    snap_t s;
    msh_lock();
    bool any = snapshot(&s);
    msh_unlock();
    if (any) write_snap(&s);
}

/* ---- the link ---- */

bool msh_target_load(msh_target_t *t)
{
    char v[160];
    memset(t, 0, sizeof *t);
    if (!aos_hal_pref_get_str("malla_link", v, sizeof v) || !v[0]) return false;
    char *f[5] = { 0 };
    int n = 0;
    for (char *p = v; n < 5;) {
        f[n++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }
    if (!strcmp(f[0], "tcp") && n >= 3) {
        t->kind = MSH_LINK_TCP;
        msh_scpy(t->host, sizeof t->host, f[1]);
        t->tcp_port = (int)strtol(f[2], NULL, 10);
    } else if (!strcmp(f[0], "ser") && n >= 3) {
        t->kind = MSH_LINK_SERIAL;
        msh_scpy(t->port, sizeof t->port, f[1]);
        t->baud = (uint32_t)strtoul(f[2], NULL, 10);
    } else if (!strcmp(f[0], "ble") && n >= 3) {
        unsigned a[6];
        if (sscanf(f[1], "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) return false;
        t->kind = MSH_LINK_BLE;
        for (int i = 0; i < 6; i++) t->addr[i] = (uint8_t)a[i];
        t->addr_type = (uint8_t)strtoul(f[2], NULL, 10);
        if (n >= 4) msh_scpy(t->name, sizeof t->name, f[3]);
    } else {
        return false;
    }
    return true;
}

void msh_target_save(const msh_target_t *t)
{
    char v[160] = "";
    switch (t->kind) {
    case MSH_LINK_TCP:
        snprintf(v, sizeof v, "tcp|%.63s|%d", t->host, t->tcp_port);
        break;
    case MSH_LINK_SERIAL:
        snprintf(v, sizeof v, "ser|%.15s|%lu", t->port, (unsigned long)t->baud);
        break;
    case MSH_LINK_BLE:
        snprintf(v, sizeof v, "ble|%02x:%02x:%02x:%02x:%02x:%02x|%u|%.31s", t->addr[0], t->addr[1], t->addr[2],
                 t->addr[3], t->addr[4], t->addr[5], t->addr_type, t->name);
        break;
    default:
        break;
    }
    aos_hal_pref_set_str("malla_link", v);
}

void msh_target_text(const msh_target_t *t, char *out, size_t cap)
{
    switch (t->kind) {
    case MSH_LINK_TCP:
        snprintf(out, cap, "TCP %.63s:%d", t->host, t->tcp_port);
        break;
    case MSH_LINK_SERIAL:
        snprintf(out, cap, "%.15s %lu", t->port, (unsigned long)t->baud);
        break;
    case MSH_LINK_BLE:
        if (t->name[0]) snprintf(out, cap, "BLE %.31s", t->name);
        else snprintf(out, cap, "BLE %02X:%02X:%02X:%02X:%02X:%02X", t->addr[0], t->addr[1], t->addr[2], t->addr[3],
                      t->addr[4], t->addr[5]);
        break;
    default:
        if (cap) out[0] = 0;
        break;
    }
}
