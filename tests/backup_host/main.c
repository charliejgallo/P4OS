/*
 * The firmware's backups (components/aos_backup/aos_backup.c) on the Mac,
 * from the command line: each argument is a step, done in order.
 *
 *   make            a plain backup          make=<passphrase>   a sealed one
 *   restore=<name>  or restore=<name>:<passphrase>
 *   list            the backups on the card, a line each
 *   missing=<name>  the apps it lists that the card does not have
 *   bg-make         through the module's thread, waited for
 *   auto            the day's copy, when one is needed     fw   the one before a firmware
 *   key=<passphrase>  seals the automatic copies from now on (key= takes it off)
 *   offer           whether this board would be offered a backup, and which
 *   offer-done      the person answered
 *   bg-fw           the copy before a firmware, as the update calls it
 *   folders         the card's data folders and whether each goes to a pendrive
 *   folder=<name>:<0|1>   chooses one
 *   to-drive        the backups and the chosen folders to the pendrive (P4_USB)
 *   from-drive      and back
 *   srv=<host>:<port>:<user>:<folder>   the server the sealed copies go to
 *   srv-key         the board's line for the server's authorized_keys
 *   srv-key-new     another key in the place of the board's
 *   look            who answers there: its key, and whether the board gets in
 *   trust           the key the last look saw is the server's from now on
 *   send            the sealed backups the server lacks, now
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
            static aos_backup_info_t all[40];
            int n = aos_backup_list(all, 40);
            for (int k = 0; k < n; k++)
                printf("backup %s made=%u fw=%s sealed=%d before=%d prefs=%d left_out=%d apps=%d why=%d\n", all[k].name, (unsigned)all[k].made,
                       all[k].fw, all[k].sealed, all[k].before, all[k].prefs, all[k].left_out, all[k].apps, all[k].why);
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
        } else if (!strcmp(a, "auto") || !strcmp(a, "fw")) {
            bool made = false;
            int rc = aos_backup_auto_now(a[0] == 'f' ? AOS_BACKUP_BEFORE_FW : AOS_BACKUP_DAILY, &made, why, sizeof why);
            printf("%s kind=%d made=%d day=%u %s\n", rc ? "FAILED" : "ok", rc, made, (unsigned)aos_backup_auto_day(), why);
        } else if (!strcmp(a, "key")) {
            bool ok = aos_backup_auto_passphrase_now(v);
            printf("%s kind=%d sealed=%d\n", ok ? "ok" : "FAILED", ok ? 0 : 1, aos_backup_auto_sealed());
        } else if (!strcmp(a, "offer")) {
            aos_backup_info_t one;
            bool yes = aos_backup_offer(&one);
            printf("offer %d %s\n", yes, yes ? one.name : "");
        } else if (!strcmp(a, "offer-done")) {
            aos_backup_offer_done();
        } else if (!strcmp(a, "bg-fw")) {
            aos_backup_status_t st;
            aos_backup_before_firmware();
            aos_backup_status(&st);
            printf("%s kind=%d busy=%d job=%s %s\n", st.last_ok ? "ok" : "FAILED", st.last_kind, st.busy, st.last_job, st.last);
        } else if (!strcmp(a, "folders")) {
            static aos_backup_folder_t all[AOS_BACKUP_FOLDERS_MAX];
            int n = aos_backup_folders(all, AOS_BACKUP_FOLDERS_MAX);
            printf("folders");
            for (int k = 0; k < n; k++) printf(" %s=%d", all[k].name, all[k].on);
            printf("\n");
        } else if (!strcmp(a, "folder")) {
            char *on = v ? strchr(v, ':') : NULL;
            if (on) { *on++ = 0; aos_backup_folder_set(v, *on == '1'); }
        } else if (!strcmp(a, "to-drive") || !strcmp(a, "from-drive")) {
            int rc = aos_backup_copy_now(a[0] == 't', why, sizeof why);
            printf("%s kind=%d %s\n", rc ? "FAILED" : "ok", rc, why);
        } else if (!strcmp(a, "srv")) {
            char *port = v ? strchr(v, ':') : NULL, *user = port ? strchr(port + 1, ':') : NULL, *dir = user ? strchr(user + 1, ':') : NULL;
            if (!dir) return 2;
            *port++ = 0; *user++ = 0; *dir++ = 0;
            aos_backup_server_set(v, atoi(port), user, dir);
            aos_backup_server_t sv;
            aos_backup_server_get(&sv);
            printf("server %s:%d %s %s on=%d pinned=%d fp=%s last=%u\n", sv.host, sv.port, sv.user, sv.dir, sv.on, sv.pinned, sv.fp, (unsigned)sv.last);
        } else if (!strcmp(a, "srv-key-new")) {
            printf("keynew %d\n", aos_backup_server_key_new());
        } else if (!strcmp(a, "srv-key")) {
            char line[200] = "";
            bool ok = aos_backup_server_key_line(line, sizeof line);
            printf("keyline %d %s\n", ok, line);
        } else if (!strcmp(a, "look")) {
            char fp[80] = "";
            bool in = false;
            int rc = aos_backup_server_look_now(why, sizeof why);
            bool seen = aos_backup_server_seen(fp, sizeof fp, &in);
            printf("%s kind=%d seen=%d in=%d fp=%s %s\n", rc ? "FAILED" : "ok", rc, seen, in, fp, why);
        } else if (!strcmp(a, "trust")) {
            aos_backup_server_t sv;
            bool ok = aos_backup_server_trust();
            aos_backup_server_get(&sv);
            printf("trust %d pinned=%d fp=%s\n", ok, sv.pinned, sv.fp);
        } else if (!strcmp(a, "send")) {
            aos_backup_server_t sv;
            int rc = aos_backup_server_send_now(why, sizeof why);
            aos_backup_server_get(&sv);
            printf("%s kind=%d last=%d %s\n", rc ? "FAILED" : "ok", rc, sv.last != 0, why);
        } else {
            fprintf(stderr, "what is %s?\n", a);
            return 2;
        }
    }
    return 0;
}
