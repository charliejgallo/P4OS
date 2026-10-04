/*
 * P4OS - the Red app's service (aos_nettools.c): ping, the LAN sweep, a
 * host's port scan, the WiFi survey and the mDNS browse, each on a thread of
 * its own. The screen only reads what they leave, under nt_lock(), and asks
 * for work with the nt_*_start() calls. Every thread stops by itself about
 * three seconds after the screen stops calling nt_keepalive(): leaving the
 * app stops the network traffic, a turn of the screen does not.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aos_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NT_IDLE = 0, NT_BUSY, NT_DONE, NT_FAILED };

bool nt_init(void);             /* from the screen, before anything; false: no memory */
void *nt_big_calloc(size_t n, size_t size);     /* PSRAM on the board */
void nt_lock(void);
void nt_unlock(void);
void nt_keepalive(void);        /* from the screen's timer */

/* "192.168.1.7" from a host-order address */
void nt_ip_str(uint32_t ip, char *out, size_t n);

/* ---- ping ---- */

#define NT_PING_HIST 120            /* two minutes at one a second */

typedef struct {
    int      state;             /* NT_BUSY while it pings */
    char     host[64];          /* as typed */
    char     ip[16];            /* what it resolved to */
    bool     resolved;
    uint32_t dns_ms;
    bool     icmp;              /* false: TCP connect time to tcp_port */
    uint16_t tcp_port;
    char     err[96];
    uint32_t sent, recv;
    float    last_ms, min_ms, max_ms, avg_ms, jitter_ms;
    int      ttl;               /* of the last reply, 0 unknown */
    float    hist[NT_PING_HIST];    /* oldest first; < 0 is a lost one */
    int      hist_n;
    uint32_t seq;               /* bumps on every change */
} nt_ping_t;

bool nt_ping_start(const char *host);
void nt_ping_stop(void);
const nt_ping_t *nt_ping(void);             /* under nt_lock() */

/* ---- the LAN sweep ---- */

#define NT_MAX_HOSTS   96
#define NT_HOST_PORTS  12

typedef struct {
    uint32_t ip;                /* host order */
    char     name[64];          /* mDNS or reverse DNS, "" if none */
    uint16_t ports[NT_HOST_PORTS];
    int      nports;
    uint32_t mdns;              /* NT_MD_* bits of what it announces */
    bool     icmp;              /* answered the ping */
    float    rtt_ms;
    bool     self;              /* this device */
} nt_host_t;

typedef struct {
    int      state;
    int      phase;             /* 0 ping, 1 ports, 2 names */
    int      done, total;       /* of the current phase */
    bool     full;              /* also the ones that do not answer ping */
    bool     icmp;              /* the ping phase could run */
    char     range[40];         /* "192.168.1.0/24" */
    char     err[96];
    nt_host_t hosts[NT_MAX_HOSTS];
    int      n;
    uint32_t elapsed_ms;
    char     file[48];          /* where the finished sweep was saved, "" if not */
    uint32_t seq;
} nt_scan_t;

extern const uint16_t NT_KNOWN_PORTS[];
extern const int NT_KNOWN_N;

/* A finished sweep is saved to aos_hal_path_scans() as one NDJSON file,
 * with the networks around scanned at its end: the portal's Red page reads
 * them. The newest NT_SAVED_MAX are kept. */
#define NT_SAVED_MAX   40

bool nt_scan_start(bool full);
void nt_scan_stop(void);
const nt_scan_t *nt_scan(void);             /* under nt_lock() */

/* A guess of what a host is, from its ports and what it announces (Spanish,
 * N_()-marked: pass it through aos_tr()). */
const char *nt_guess(const nt_host_t *h);
/* The service usually behind a TCP port, "" if none known. */
const char *nt_port_name(uint16_t port);

/* ---- one host's ports ---- */

#define NT_MAX_OPEN 64

typedef struct {
    int      state;
    uint32_t ip;
    int      done, total;
    uint16_t open[NT_MAX_OPEN];
    int      nopen;
    int      closed;            /* answered "closed": the host is there */
    uint32_t elapsed_ms;
    char     err[96];
    uint32_t seq;
} nt_ports_t;

bool nt_ports_start(uint32_t ip);
void nt_ports_stop(void);
const nt_ports_t *nt_ports(void);           /* under nt_lock() */

/* ---- WiFi ---- */

#define NT_MAX_APS     48
#define NT_RSSI_HIST   240          /* two minutes at 2 Hz */

typedef struct {
    int      state;             /* of the last scan */
    aos_wifi_ap_ex_t aps[NT_MAX_APS];
    int      n;
    uint32_t scanned_ms;        /* uptime of the last scan */
    bool     cur_ok;            /* associated AP, fresh */
    aos_wifi_ap_ex_t cur;
    int8_t   rssi[NT_RSSI_HIST];    /* oldest first; 0 = no sample */
    int      rssi_n;
    uint32_t seq;
} nt_wifi_t;

void nt_wifi_run(void);         /* starts the sampler (2 Hz) if it is not running */
bool nt_wifi_scan(void);        /* one scan; false if one is under way */
const nt_wifi_t *nt_wifi(void);             /* under nt_lock() */

/* ---- mDNS ---- */

#define NT_MAX_SVC 64

enum {
    NT_MD_HA = 1u << 0, NT_MD_ESPHOME = 1u << 1, NT_MD_MQTT = 1u << 2, NT_MD_HTTP = 1u << 3,
    NT_MD_HAP = 1u << 4, NT_MD_CAST = 1u << 5, NT_MD_PRINTER = 1u << 6, NT_MD_SMB = 1u << 7,
    NT_MD_SSH = 1u << 8, NT_MD_RTSP = 1u << 9, NT_MD_AIRPLAY = 1u << 10, NT_MD_SPOTIFY = 1u << 11,
    NT_MD_ARDUINO = 1u << 12, NT_MD_WORKSTATION = 1u << 13,
};

typedef struct {
    const char *type;           /* "_esphomelib._tcp" */
    const char *label;          /* "ESPHome" (N_) */
    uint32_t bit;
} nt_mdns_type_t;

extern const nt_mdns_type_t NT_MDNS_TYPES[];
extern const int NT_MDNS_NTYPES;

typedef struct {
    int      state;
    aos_mdns_svc_t svc[NT_MAX_SVC];     /* sorted by type, then name */
    int      n;
    uint32_t elapsed_ms;
    char     err[96];
    uint32_t seq;
} nt_mdns_t;

bool nt_mdns_start(void);
const nt_mdns_t *nt_mdns(void);             /* under nt_lock() */
int nt_mdns_type_index(const char *type);   /* into NT_MDNS_TYPES, -1 */

#ifdef __cplusplus
}
#endif
