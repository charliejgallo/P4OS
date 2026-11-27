/*
 * P4OS - Mensajes: the client in a terminal, for tools/fake_telegram.py.
 *
 *     MJ_PREFS=<file> mj_cli <data dir> <command>...
 *
 * The same mj_api.c, mj_mtp.c and store the app uses, without a screen: it
 * runs the commands one after the other and prints what the store holds,
 * so a test can go from the handshake to reading a photo with no
 * simulator. Each command waits for what it asked (or up to its timeout):
 *
 *   app <api_id> <api_hash>        phone <number>      code <digits>
 *   password <text>                wait <ms>           expect <auth-state>
 *   dialogs                        open <u|c|h> <id>   history
 *   send <text>                    reply <id> <text>   photo <path>
 *   edit <id> <text>               delete <id>         react <id> <emoji>
 *   forward <id> <u|c|h> <id>      search <text>       fetch-photos
 *   fetch-avatars                  fetch-docs          logout
 *   mem                            expect-msg <text>   expect-chats <n>
 *   reload
 *
 * u, c, h: a user, a basic group (chat), a channel (h). Exits 1 at the first
 * expect that does not hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mj_api.h"
#include "mj_port.h"

static const char *auth_name(int a)
{
    static const char *n[] = {"starting", "need_api", "phone", "busy", "code", "password", "signup", "ready"};
    return a >= 0 && a <= 7 ? n[a] : "?";
}

static void wait_ms(uint32_t ms)
{
    mj_sleep_ms(ms);
}

/* Waits until cond() or the timeout; true if it held. */
static bool until(bool (*cond)(void *), void *arg, uint32_t ms)
{
    uint32_t t0 = mj_now_ms();
    while ((uint32_t)(mj_now_ms() - t0) < ms) {
        mj_store_lock();
        bool ok = cond(arg);
        mj_store_unlock();
        if (ok) return true;
        mj_sleep_ms(30);
    }
    return false;
}

static bool c_auth(void *a) { return mj_S.auth == *(int *)a; }
static bool c_not_busy(void *a)
{
    (void)a;
    return mj_S.auth != MJ_AUTH_BUSY && mj_S.auth != MJ_AUTH_STARTING;
}
static bool c_dialogs(void *a)
{
    (void)a;
    return !mj_S.dialogs_loading && mj_S.ndialogs > 0;
}
static bool c_chat(void *a)
{
    mj_pkey_t *k = (mj_pkey_t *)a;
    return mj_pkey_eq(mj_S.chat, *k) && !mj_S.chat_loading && (mj_S.nmsgs > 0 || mj_S.chat_start);
}
static bool c_search(void *a)
{
    (void)a;
    return !mj_S.searching;
}
static bool c_msg(void *a)
{
    const char *t = (const char *)a;
    for (int i = 0; i < mj_S.nmsgs; i++)
        if (strstr(mj_S.msgs[i]->text, t) && !mj_S.msgs[i]->pending) return true;
    return false;
}
static int s_want_media;
static bool c_media(void *a)
{
    (void)a;
    return mj_S.nmedia_done >= s_want_media;
}

static void print_dialogs(void)
{
    mj_store_lock();
    printf("-- %d chats, %d unread\n", mj_S.ndialogs, mj_S.unread_total);
    for (int i = 0; i < mj_S.ndialogs; i++) {
        mj_dialog_t *d = &mj_S.dialogs[i];
        printf("  %s%c %-24s %3d  %s%s%s\n", d->pinned ? "*" : " ", "?uch"[d->key.type], mj_peer_name(d->key), d->unread,
               d->who, d->who[0] ? ": " : "", d->preview);
    }
    mj_store_unlock();
}

static void print_chat(void)
{
    mj_store_lock();
    printf("-- %s: %d messages%s%s%s\n", mj_peer_name(mj_S.chat), mj_S.nmsgs, mj_S.chat_start ? " (all)" : "",
           mj_S.typing[0] ? ", typing: " : "", mj_S.typing);
    for (int i = 0; i < mj_S.nmsgs; i++) {
        mj_msg_t *m = mj_S.msgs[i];
        char line[200];
        mj_msg_line(m, line, sizeof line);
        printf("  %6d %s %-14.14s %s%s%s%s%s%s\n", m->id, m->out ? ">" : "<", mj_peer_name(m->from), line,
               m->reply_to ? " [reply]" : "", m->fwd_from[0] ? " [fwd]" : "", m->edit_date ? " [edited]" : "",
               m->reactions[0] ? "  " : "", m->reactions);
        if (m->service) printf("         (service %d %s)\n", m->action, m->action_arg);
    }
    mj_store_unlock();
}

static mj_pkey_t key_of(const char *t, const char *id)
{
    mj_pkey_t k = {*t == 'u' ? MJ_PEER_USER : *t == 'c' ? MJ_PEER_CHAT : MJ_PEER_CHANNEL, strtoll(id, NULL, 10)};
    return k;
}

static void fetch_all(int kind)
{
    int n = 0;
    mj_store_lock();
    int before = mj_S.nmedia_done;
    if (kind == MJ_FETCH_AVATAR) {
        for (int i = 0; i < mj_S.ndialogs; i++) {
            mj_peer_t *p = mj_store_peer(mj_S.dialogs[i].key);
            if (p && p->photo_id) {
                mj_api_fetch(MJ_FETCH_AVATAR, p, NULL, 0);
                n++;
            }
        }
    } else {
        for (int i = 0; i < mj_S.nmsgs; i++) {
            mj_media_t *md = mj_S.msgs[i]->media;
            if (!md || !md->id) continue;
            if (kind == MJ_FETCH_PHOTO && md->kind == MJ_MEDIA_PHOTO) {
                mj_api_fetch(MJ_FETCH_PHOTO, NULL, md, mj_S.msgs[i]->id);
                n++;
            } else if (kind == MJ_FETCH_DOC && md->kind != MJ_MEDIA_PHOTO) {
                mj_api_fetch(md->thumb_small[0] ? MJ_FETCH_THUMB : MJ_FETCH_DOC, NULL, md, mj_S.msgs[i]->id);
                n++;
            }
        }
    }
    mj_store_unlock();
    s_want_media = before + n;
    bool ok = until(c_media, NULL, 15000);
    mj_store_lock();
    printf("-- %d files asked, %d arrived%s\n", n, mj_S.nmedia_done - before, ok ? "" : " (timed out)");
    for (int i = before; i < mj_S.nmedia_done; i++) printf("  %s\n", mj_S.media_done[i]);
    mj_store_unlock();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: MJ_PREFS=<file> %s <data dir> <commands...>\n", argv[0]);
        return 2;
    }
    mj_store_init();
    mj_api_cfg_t cfg = {0};
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", argv[1]);
    snprintf(cfg.dl_dir, sizeof cfg.dl_dir, "%s/descargas", argv[1]);
    snprintf(cfg.lang, sizeof cfg.lang, "es");
    snprintf(cfg.device, sizeof cfg.device, "mj_cli");
    cfg.cache_max = 8 * 1024 * 1024;
    if (!mj_api_start(&cfg)) return 3;
    int st = MJ_AUTH_STARTING;
    while (mj_S.auth == MJ_AUTH_STARTING) wait_ms(20);
    (void)st;
    int rc = 0;
    for (int i = 2; i < argc && !rc; i++) {
        const char *c = argv[i];
        printf("> %s", c);
        int args = !strcmp(c, "app") || !strcmp(c, "reply") || !strcmp(c, "edit") || !strcmp(c, "react") ||
                           !strcmp(c, "open")
                       ? 2
                   : !strcmp(c, "forward")                                                         ? 3
                   : !strcmp(c, "dialogs") || !strcmp(c, "reload") || !strcmp(c, "history") || !strcmp(c, "mem") || !strcmp(c, "logout") ||
                           !strncmp(c, "fetch", 5)
                       ? 0
                       : 1;
        for (int k = 1; k <= args && i + k < argc; k++) printf(" %s", argv[i + k]);
        printf("\n");
        if (!strcmp(c, "app")) {
            mj_api_set_app(atoi(argv[i + 1]), argv[i + 2]);
            i += 2;
            int want = MJ_AUTH_PHONE;
            until(c_auth, &want, 3000);
        } else if (!strcmp(c, "phone")) {
            mj_api_login_phone(argv[++i]);
            wait_ms(100);
            until(c_not_busy, NULL, 30000);
        } else if (!strcmp(c, "code")) {
            mj_api_login_code(argv[++i]);
            wait_ms(100);
            until(c_not_busy, NULL, 30000);
        } else if (!strcmp(c, "password")) {
            mj_api_login_password(argv[++i]);
            wait_ms(100);
            until(c_not_busy, NULL, 60000);
        } else if (!strcmp(c, "wait")) {
            wait_ms((uint32_t)atoi(argv[++i]));
        } else if (!strcmp(c, "expect")) {
            const char *want = argv[++i];
            int w = -1;
            for (int k = 0; k <= 7; k++)
                if (!strcmp(want, auth_name(k))) w = k;
            bool ok = until(c_auth, &w, 5000);
            printf("   auth is %s%s%s\n", auth_name(mj_S.auth), mj_S.auth_err[0] ? ", error " : "", mj_S.auth_err);
            if (!ok) rc = 1;
        } else if (!strcmp(c, "reload")) {
            mj_api_reload_dialogs();
            wait_ms(1500);
        } else if (!strcmp(c, "dialogs")) {
            until(c_dialogs, NULL, 15000);
            print_dialogs();
        } else if (!strcmp(c, "expect-chats")) {
            int n = atoi(argv[++i]);
            until(c_dialogs, NULL, 15000);
            if (mj_S.ndialogs < n) {
                printf("   only %d chats\n", mj_S.ndialogs);
                rc = 1;
            }
        } else if (!strcmp(c, "open")) {
            mj_pkey_t k = key_of(argv[i + 1], argv[i + 2]);
            mj_api_open_chat(k);
            i += 2;
            until(c_chat, &k, 15000);
            print_chat();
        } else if (!strcmp(c, "history")) {
            mj_api_more_history();
            wait_ms(800);
            print_chat();
        } else if (!strcmp(c, "send")) {
            mj_api_send_text(mj_S.chat, argv[++i], 0);
            wait_ms(1200);
            print_chat();
        } else if (!strcmp(c, "reply")) {
            mj_api_send_text(mj_S.chat, argv[i + 2], atoi(argv[i + 1]));
            i += 2;
            wait_ms(1200);
            print_chat();
        } else if (!strcmp(c, "photo")) {
            mj_api_send_photo(mj_S.chat, argv[++i], "foto de prueba", 0);
            wait_ms(3000);
            print_chat();
        } else if (!strcmp(c, "edit")) {
            mj_api_edit(mj_S.chat, atoi(argv[i + 1]), argv[i + 2]);
            i += 2;
            wait_ms(1000);
            print_chat();
        } else if (!strcmp(c, "delete")) {
            mj_api_delete(mj_S.chat, atoi(argv[++i]), true);
            wait_ms(1000);
            print_chat();
        } else if (!strcmp(c, "react")) {
            mj_api_react(mj_S.chat, atoi(argv[i + 1]), argv[i + 2]);
            i += 2;
            wait_ms(1000);
            print_chat();
        } else if (!strcmp(c, "forward")) {
            mj_api_forward(mj_S.chat, atoi(argv[i + 1]), key_of(argv[i + 2], argv[i + 3]));
            i += 3;
            wait_ms(1200);
            print_dialogs();
        } else if (!strcmp(c, "search")) {
            mj_api_search(argv[++i]);
            wait_ms(100);
            until(c_search, NULL, 10000);
            mj_store_lock();
            printf("-- %d peers, %d messages\n", mj_S.nfound_peers, mj_S.nfound);
            for (int k = 0; k < mj_S.nfound_peers; k++) printf("  peer %s\n", mj_peer_name(mj_S.found_peers[k]));
            for (int k = 0; k < mj_S.nfound; k++)
                printf("  msg %d in %s: %s\n", mj_S.found[k]->id, mj_peer_name(mj_S.found[k]->chat), mj_S.found[k]->text);
            mj_store_unlock();
        } else if (!strcmp(c, "expect-msg")) {
            const char *t = argv[++i];
            bool ok = until(c_msg, (void *)t, 15000);
            print_chat();
            if (!ok) {
                printf("   no message with \"%s\"\n", t);
                rc = 1;
            }
        } else if (!strcmp(c, "fetch-photos")) {
            fetch_all(MJ_FETCH_PHOTO);
        } else if (!strcmp(c, "fetch-avatars")) {
            fetch_all(MJ_FETCH_AVATAR);
        } else if (!strcmp(c, "fetch-docs")) {
            fetch_all(MJ_FETCH_DOC);
        } else if (!strcmp(c, "logout")) {
            mj_api_logout();
            wait_ms(1500);
        } else if (!strcmp(c, "mem")) {
            printf("   memory held: %u KB (peak %u KB)\n", (unsigned)(mj_mem_used() / 1024),
                   (unsigned)(mj_mem_peak() / 1024));
        } else {
            printf("   unknown command\n");
            rc = 2;
        }
    }
    mj_api_stop(5000);
    mj_store_lock();
    mj_store_free();
    mj_store_unlock();
    printf("-- done (%s); memory left %u bytes, peak %u KB\n", rc ? "FAILED" : "ok", (unsigned)mj_mem_used(),
           (unsigned)(mj_mem_peak() / 1024));
    return rc;
}
