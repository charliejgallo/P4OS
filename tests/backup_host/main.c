/*
 * The firmware's backups (components/aos_backup/aos_backup.c) on the Mac,
 * from the command line: each argument is a step, done in order.
 *
 *   make            a plain backup          make=<passphrase>   a sealed one
 *   restore=<name>  or restore=<name>:<passphrase>
 *   list            the backups on the card, a line each
 *   missing=<name>  the apps it lists that the card does not have
 *   bg-make         through the module's thread, waited for
 *
 * A job prints "ok|FAILED kind=<n> <a line>".
 */
#include "aos_backup.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *v = strchr(a, '=');
        char why[160] = "", name[AOS_BACKUP_NAME_MAX] = "";
        if (v) *v++ = 0;
        if (!strcmp(a, "make")) {
            int rc = aos_backup_make_now(v, false, name, sizeof name, why, sizeof why);
            printf("%s kind=%d %s\n", rc ? "FAILED" : "ok", rc, why);
            if (!rc) printf("made %s\n", name);
        } else if (!strcmp(a, "restore")) {
            char *pass = v ? strchr(v, ':') : NULL;
            int written = 0;
            if (pass) *pass++ = 0;
            int rc = aos_backup_restore_now(v ? v : "", pass, &written, why, sizeof why);
            printf("%s kind=%d written=%d %s\n", rc ? "FAILED" : "ok", rc, written, why);
        } else if (!strcmp(a, "list")) {
            static aos_backup_info_t all[16];
            int n = aos_backup_list(all, 16);
            for (int k = 0; k < n; k++)
                printf("backup %s made=%u fw=%s sealed=%d before=%d prefs=%d left_out=%d apps=%d\n", all[k].name, (unsigned)all[k].made,
                       all[k].fw, all[k].sealed, all[k].before, all[k].prefs, all[k].left_out, all[k].apps);
        } else if (!strcmp(a, "missing")) {
            static aos_backup_app_t apps[32];
            int n = aos_backup_missing_apps(v ? v : "", apps, 32);
            for (int k = 0; k < n; k++) printf("missing %s [%s]\n", apps[k].file, apps[k].packs);
        } else if (!strcmp(a, "bg-make")) {
            aos_backup_status_t st;
            bool started = aos_backup_make(v);
            bool twice = aos_backup_make(v);
            do { aos_hal_sleep_ms(20); aos_backup_status(&st); } while (st.busy);
            printf("%s kind=%d started=%d twice=%d %s\n", st.last_ok ? "ok" : "FAILED", st.last_kind, started, twice, st.last);
            if (st.last_ok) printf("made %s\n", st.name);
        } else {
            fprintf(stderr, "what is %s?\n", a);
            return 2;
        }
    }
    return 0;
}
