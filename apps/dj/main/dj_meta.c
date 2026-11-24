/*
 * DJ - what the app remembers per track: its tempo and beat grid (so the
 * analysis need not guess again, and a grid fixed by hand stays fixed), its
 * cue and its hot cues.
 *
 * One text file, <data>/dj/tracks.txt, a line per track:
 *
 *   name<TAB>size<TAB>bpm<TAB>first beat<TAB>cue<TAB>hot1<TAB>..<TAB>hot4<TAB>by hand
 *
 * the positions in the track's own samples, -1 for an empty hot cue. The
 * tempo and the grid are taken back only when they were set by hand (the
 * last field, 1): a detected one is detected again, so a better detector
 * in a later version is not hidden by an old guess. A track
 * is known by its file name and size (moving it to another folder keeps it;
 * a new version of it does not). Small: read whole, rewritten whole.
 */
#include "dj.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLINES 2000

static const char *base(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static void file_path(char *out, size_t n)
{
    snprintf(out, n, "%s/tracks.txt", dj_data_dir());
}

/* splits 'line' at the tabs; returns how many fields */
static int fields(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    char *nl = strchr(f[n - 1], '\n');
    if (nl) *nl = 0;
    return n;
}

bool dj_meta_load(dj_track_t *t)
{
    char path[200];
    file_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[400];
    const char *name = base(t->path);
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        char *fl[10];
        int fields_n = fields(line, fl, 10);
        if (fields_n < 9) continue;
        if (strcmp(fl[0], name) || (uint32_t)strtol(fl[1], NULL, 10) != t->fsize) continue;
        float bpm = strtof(fl[2], NULL);
        bool hand = fields_n > 9 && fl[9][0] == '1';
        if (hand && bpm >= 40.0f && bpm <= 300.0f) {
            t->bpm = bpm;
            t->beat0 = strtof(fl[3], NULL);
            found = true;
        }
        t->cue = strtof(fl[4], NULL);
        if (t->cue < 0.0f || t->cue >= (float)t->nsamp) t->cue = 0.0f;
        for (int h = 0; h < DJ_HOT; h++) {
            t->hot[h] = strtof(fl[5 + h], NULL);
            if (t->hot[h] >= (float)t->nsamp) t->hot[h] = -1.0f;
        }
        break;
    }
    fclose(f);
    return found;
}

/* the tempo, for the library: 0 if the track is not known */
float dj_meta_bpm(const char *name, uint32_t size, char *cache, int cache_len)
{
    (void)cache_len;
    if (!cache) return 0.0f;
    /* the cache is the file's text, read once by the library */
    const char *p = cache;
    size_t nl = strlen(name);
    while (p && *p) {
        if (!strncmp(p, name, nl) && p[nl] == '\t' && (uint32_t)strtol(p + nl + 1, NULL, 10) == size) {
            const char *q = strchr(p + nl + 1, '\t');
            return q ? strtof(q + 1, NULL) : 0.0f;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0.0f;
}

char *dj_meta_read_all(int *len)
{
    char path[200];
    file_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512 * 1024) { fclose(f); return NULL; }
    char *b = dj_alloc((size_t)n + 1);
    if (b) {
        n = (long)fread(b, 1, (size_t)n, f);
        b[n] = 0;
        if (len) *len = (int)n;
    }
    fclose(f);
    return b;
}

/* writes (or replaces) the line of the track */
void dj_meta_save(const char *path, uint32_t size, float bpm, float beat0, float cue, const float *hot, bool hand)
{
    char fp[200], tmp[210];
    file_path(fp, sizeof fp);
    snprintf(tmp, sizeof tmp, "%s.part", fp);
    const char *name = base(path);
    int len = 0;
    char *old = dj_meta_read_all(&len);
    FILE *o = fopen(tmp, "w");
    if (!o) { dj_free(old); return; }
    char line[400];
    snprintf(line, sizeof line, "%s\t%u\t%.3f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%d\n", name, (unsigned)size, (double)bpm,
             (double)beat0, (double)cue, (double)hot[0], (double)hot[1], (double)hot[2], (double)hot[3], hand ? 1 : 0);
    fputs(line, o);
    int kept = 1;
    if (old) {
        char *p = old;
        size_t nl = strlen(name);
        while (*p && kept < MAXLINES) {
            char *e = strchr(p, '\n');
            size_t ll = e ? (size_t)(e - p + 1) : strlen(p);
            bool same = !strncmp(p, name, nl) && p[nl] == '\t' && (uint32_t)strtol(p + nl + 1, NULL, 10) == size;
            if (!same && ll > 1) {
                fwrite(p, 1, ll, o);
                if (!e) fputs("\n", o);
                kept++;
            }
            p += ll;
        }
    }
    fclose(o);
    dj_free(old);
    remove(fp);
    rename(tmp, fp);
}
