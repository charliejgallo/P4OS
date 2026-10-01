/*
 * P4OS simulator - a folder in one pass (aos_hal_dir_scan, aos_hal.h).
 * The Mac's file system has no O(n^2) stat, so here it is readdir + stat;
 * the point is that Archivos calls the same function as on the board.
 */
#include "aos_hal.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

int aos_hal_dir_scan(const char *path, aos_dir_cb_t cb, void *ctx)
{
    if (!path || !cb) return -1;
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    char full[1024];
    while ((e = readdir(d)) != NULL) {
        aos_dir_entry_t de = { .name = e->d_name };
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (!stat(full, &st)) {
            de.dir = S_ISDIR(st.st_mode);
            de.size = de.dir ? 0 : (uint32_t)st.st_size;
            de.mtime = st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0;
        }
        n++;
        if (!cb(&de, ctx)) break;
    }
    closedir(d);
    return n;
}
