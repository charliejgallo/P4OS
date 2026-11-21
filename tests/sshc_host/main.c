/*
 * The firmware's SSH and SFTP client (components/aos_ssh/aos_sshc.c) on the
 * Mac, from the command line, against a server it is pointed at: the
 * simulator's own (tests/sshc_host/run.sh).
 *
 *   t <host> <port> <user> <seed, 64 hex digits> <kept host key or -> <step>...
 *
 *   key                 the line for authorized_keys, and nothing else
 *   mkdirs=<path>   put=<local>:<remote>   stat=<path>   list=<path>   rm=<path>
 *
 * It prints "host <fingerprint> <key>" once in, a line a step, and "ok" or
 * "FAILED kind=<n> <why>" at the end.
 */
#include "aos_sshc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void each(void *ctx, const char *name, bool dir, uint64_t size, uint32_t mtime)
{
    (void)ctx; (void)mtime;
    printf("name %s %s %llu\n", dir ? "d" : "f", name, (unsigned long long)size);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 6) return 2;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        if (sscanf(argv[4] + 2 * i, "%2x", &v) != 1) return 2;
        seed[i] = (uint8_t)v;
    }
    if (argc > 6 && !strcmp(argv[6], "key")) {
        char line[200];
        aos_sshc_key_line(seed, "p4os@prueba", line, sizeof line);
        printf("%s\n", line);
        return 0;
    }
    aos_sshc_opts_t o = { argv[1], atoi(argv[2]), argv[3], seed, strcmp(argv[5], "-") ? argv[5] : NULL, NULL };
    char why[160] = "", seen[600] = "";
    int err = 0;
    aos_sshc_t *c = aos_sshc_open(&o, &err, why, sizeof why, seen);
    if (!c) {
        printf("FAILED kind=%d %s\n", err, why);
        if (seen[0]) printf("seen %s\n", seen);
        return 1;
    }
    printf("host %s %s\n", aos_sshc_fingerprint(c), aos_sshc_hostkey(c));
    err = argc > 6 ? aos_sshc_sftp(c) : 0;
    for (int i = 6; i < argc && !err; i++) {
        char *a = argv[i], *v = strchr(a, '=');
        if (v) *v++ = 0;
        if (!v) continue;
        if (!strcmp(a, "mkdirs")) err = aos_sshc_mkdirs(c, v);
        else if (!strcmp(a, "put")) {
            char *remote = strchr(v, ':');
            if (!remote) return 2;
            *remote++ = 0;
            err = aos_sshc_put(c, v, remote);
        } else if (!strcmp(a, "stat")) {
            bool there = false;
            uint64_t size = 0;
            err = aos_sshc_stat(c, v, &there, &size);
            if (!err) printf("stat %s %d %llu\n", v, there, (unsigned long long)size);
        } else if (!strcmp(a, "list")) {
            int rc = aos_sshc_list(c, v, each, NULL);
            printf("listed %s %d\n", v, rc);
        } else if (!strcmp(a, "rm")) err = aos_sshc_remove(c, v);
        else return 2;
        if (!err) printf("did %s\n", a);
    }
    if (err) printf("FAILED kind=%d %s\n", err, aos_sshc_why(c));
    else printf("ok\n");
    aos_sshc_close(c);
    return err ? 1 : 0;
}
