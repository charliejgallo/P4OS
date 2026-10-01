/*
 * P4OS - Radio (from AmoledOS): the list of stations. See radio_lib.h.
 *
 * The JSON is the portal's own, so the reader is small on purpose: it walks
 * the objects inside "stations" and takes the six keys it knows, undoing the
 * escapes JSON.stringify can produce (\" \\ \/ and \uXXXX, the last to
 * UTF-8). Anything it does not understand is skipped, never guessed.
 */
#include "radio_lib.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

#define LIB_FILE_MAX    (64 * 1024)

typedef struct {
    const char *name, *url, *cc, *codec;
    uint16_t    kbps;
    bool        hls;
} lib_default_t;

/* The portal's DEFAULTS (components/aos_web/radio.html in AmoledOS). */
static const lib_default_t DEFAULTS[] = {
    { "Metro 95.1",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/METRO.mp3",
      "AR", "MP3", 96, false },
    { "Aspen 102.3",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/ASPEN.mp3",
      "AR", "MP3", 96, false },
    { "La 100",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/FM999_56.mp3",
      "AR", "MP3", 96, false },
    { "Rock & Pop 95.9",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/ROCKANDPOP.mp3",
      "AR", "MP3", 96, false },
    { "Radio Rivadavia",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/RIVADAVIA.mp3",
      "AR", "MP3", 96, false },
    { "Radio Disney Argentina",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/DISNEY_ARG_BA.mp3",
      "AR", "MP3", 128, false },
    { "FM Blackie 89.1",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/BLACKIE_89_1.mp3",
      "AR", "MP3", 96, false },
    { "Nacional Rock 93.7",
      "https://sa.mp3.icecast.magma.edge-access.net/sc_rad39",
      "AR", "MP3", 192, false },
    { "Nacional Folklórica 98.7",
      "https://sa.mp3.icecast.magma.edge-access.net/sc_rad38",
      "AR", "MP3", 160, false },
    { "Nacional Clásica 96.7",
      "https://sa.mp3.icecast.magma.edge-access.net/sc_rad37",
      "AR", "MP3", 192, false },
    { "Radio Nacional AM 870",
      "https://sa.mp3.icecast.magma.edge-access.net/sc_rad1",
      "AR", "MP3", 56, false },
    { "La Popu",
      "https://liveradio.mediainbox.net/popular.mp3",
      "AR", "MP3", 96, false },
    { "La Nación +Música",
      "https://stream.radio.co/s2ed3bec0a/listen",
      "AR", "MP3", 128, false },
    { "Radio Mitre",
      "http://playerservices.streamtheworld.com/api/livestream-redirect/AM790_56AAC_SC",
      "AR", "AAC+", 64, false },
    { "Radio 10",
      "https://radio10.stweb.tv/radio10/live/playlist.m3u8",
      "AR", "AAC", 65, true },
    { "Radio Con Vos 89.9",
      "https://server1.stweb.tv/rcvos/live/playlist.m3u8",
      "AR", "AAC", 49, true },
    { "La Red AM 910",
      "https://playerservices.streamtheworld.com/api/livestream-redirect/LA_RED_AM910AAC.aac",
      "AR", "AAC+", 64, false },
    { "El Destape",
      "https://ipanel.instream.audio/8004/stream",
      "AR", "AAC", 48, false },
    { "SomaFM Groove Salad",
      "https://ice1.somafm.com/groovesalad-128-mp3",
      "US", "MP3", 128, false },
    { "SomaFM Drone Zone",
      "https://ice1.somafm.com/dronezone-128-mp3",
      "US", "MP3", 128, false },
    { "SomaFM Secret Agent",
      "https://ice1.somafm.com/secretagent-128-mp3",
      "US", "MP3", 128, false },
    { "Radio Paradise",
      "http://stream.radioparadise.com/mp3-128",
      "US", "MP3", 128, false },
    { "FIP",
      "https://icecast.radiofrance.fr/fip-midfi.mp3",
      "FR", "MP3", 128, false },
    { "FIP Jazz",
      "https://icecast.radiofrance.fr/fipjazz-midfi.mp3",
      "FR", "MP3", 128, false },
    { "KEXP Seattle",
      "https://kexp-mp3-128.streamguys1.com/kexp128.mp3",
      "US", "MP3", 128, false },
    { "Radio Swiss Jazz",
      "http://stream.srg-ssr.ch/m/rsj/mp3_128",
      "CH", "MP3", 128, false },
    { "Radio Swiss Classic",
      "http://stream.srg-ssr.ch/m/rsc_de/mp3_128",
      "CH", "MP3", 128, false },
    { "Radio Swiss Pop",
      "http://stream.srg-ssr.ch/m/rsp/mp3_128",
      "CH", "MP3", 128, false },
    { "WFMU",
      "https://stream0.wfmu.org/freeform-128k",
      "US", "MP3", 128, false },
    { "Deutschlandfunk",
      "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3",
      "DE", "MP3", 128, false },
    { "Radio Deejay",
      "https://4c4b867c89244861ac216426883d1ad0.msvdn.net/radiodeejay/radiodeejay/master_ma.m3u8",
      "IT", "AAC+", 122, true },
    { "France Inter",
      "https://stream.radiofrance.fr/franceinter/franceinter_hifi.m3u8?id=radiofrance",
      "FR", "AAC", 0, true },
    { "Onda Cero",
      "https://atres-live.ondacero.es/live/ondacero/bitrate_1.m3u8",
      "ES", "AAC", 128, true },
};

static void *big_alloc(size_t n)
{
#if !defined(AOS_SIM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    if (n == cap - 1) {
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;                    /* not half a UTF-8 character */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void put_utf8(char *out, size_t cap, size_t *o, unsigned cp)
{
    char b[4];
    int n;
    if (cp < 0x80) {
        b[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    }
    if (*o + (size_t)n < cap) {
        memcpy(out + *o, b, (size_t)n);
        *o += (size_t)n;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The value of "key" inside [obj, end): a string (unescaped) or a bare
 * number / true / false. False if absent. */
static bool json_value(const char *obj, const char *end, const char *key,
                       char *out, size_t cap)
{
    char k[24];
    snprintf(k, sizeof(k), "\"%s\"", key);
    size_t kl = strlen(k);
    for (const char *p = obj; p + kl < end; p++) {
        if (memcmp(p, k, kl) != 0) {
            continue;
        }
        const char *v = p + kl;
        while (v < end && (*v == ' ' || *v == ':' || *v == '\t' || *v == '\n' || *v == '\r')) {
            v++;
        }
        size_t o = 0;
        if (v < end && *v == '"') {
            v++;
            while (v < end && *v != '"') {
                if (*v == '\\' && v + 1 < end) {
                    v++;
                    if (*v == 'u' && v + 4 < end) {
                        int h[4] = { hexval(v[1]), hexval(v[2]), hexval(v[3]), hexval(v[4]) };
                        if (h[0] >= 0 && h[1] >= 0 && h[2] >= 0 && h[3] >= 0) {
                            put_utf8(out, cap, &o, (unsigned)(h[0] << 12 | h[1] << 8 | h[2] << 4 | h[3]));
                        }
                        v += 5;
                        continue;
                    }
                    char c = *v == 'n' ? ' ' : (*v == 't' ? ' ' : *v);
                    if (o + 1 < cap) out[o++] = c;
                    v++;
                    continue;
                }
                if (o + 1 < cap) out[o++] = *v;
                v++;
            }
        } else {
            while (v < end && *v != ',' && *v != '}' && *v != ' ' && o + 1 < cap) {
                out[o++] = *v++;
            }
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

/* Where the object that starts at 'p' ends, strings respected. */
static const char *object_end(const char *p, const char *end)
{
    int depth = 0;
    bool str = false;
    for (; p < end; p++) {
        if (str) {
            if (*p == '\\') p++;
            else if (*p == '"') str = false;
            continue;
        }
        if (*p == '"') str = true;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p + 1;
    }
    return NULL;
}

static int parse(radio_lib_t *lib, const char *txt, size_t len)
{
    const char *end = txt + len;
    const char *p = strstr(txt, "\"stations\"");
    if (!p) {
        return 0;
    }
    p = strchr(p, '[');
    if (!p) {
        return 0;
    }
    int n = 0;
    char buf[300];
    while (p < end && n < RADIO_LIB_MAX) {
        while (p < end && *p != '{' && *p != ']') {
            p++;
        }
        if (p >= end || *p == ']') {
            break;
        }
        const char *e = object_end(p, end);
        if (!e) {
            break;
        }
        radio_lib_station_t *s = &lib->s[n];
        memset(s, 0, sizeof(*s));
        if (json_value(p, e, "url", buf, sizeof(buf)) && buf[0]) {
            copy_str(s->url, sizeof(s->url), buf);
            if (json_value(p, e, "name", buf, sizeof(buf))) {
                copy_str(s->name, sizeof(s->name), buf);
            }
            if (!s->name[0]) {
                copy_str(s->name, sizeof(s->name), s->url);
            }
            if (json_value(p, e, "cc", buf, sizeof(buf))) {
                copy_str(s->cc, sizeof(s->cc), buf);
            }
            if (json_value(p, e, "codec", buf, sizeof(buf))) {
                copy_str(s->codec, sizeof(s->codec), buf);
            }
            if (json_value(p, e, "kbps", buf, sizeof(buf))) {
                s->kbps = (uint16_t)atoi(buf);
            }
            if (json_value(p, e, "hls", buf, sizeof(buf))) {
                s->hls = strcmp(buf, "true") == 0;
            }
            n++;
        }
        p = e;
    }
    return n;
}

static bool load_card(radio_lib_t *lib)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) {
        return false;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/radio/library.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char *txt = big_alloc(LIB_FILE_MAX + 1);
    size_t len = txt ? fread(txt, 1, LIB_FILE_MAX, f) : 0;
    fclose(f);
    if (!txt) {
        return false;
    }
    txt[len] = '\0';
    int n = len > 0 ? parse(lib, txt, len) : 0;
    free(txt);
    if (n <= 0) {
        return false;
    }
    lib->count = n;
    lib->from_card = true;
    return true;
}

bool radio_lib_load(radio_lib_t *lib)
{
    if (!lib->s) {
        lib->s = big_alloc(sizeof(radio_lib_station_t) * RADIO_LIB_MAX);
        if (!lib->s) {
            return false;
        }
    }
    lib->count = 0;
    lib->from_card = false;
    if (load_card(lib)) {
        return true;
    }
    int n = (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0]));
    for (int i = 0; i < n && i < RADIO_LIB_MAX; i++) {
        radio_lib_station_t *s = &lib->s[i];
        memset(s, 0, sizeof(*s));
        copy_str(s->name, sizeof(s->name), DEFAULTS[i].name);
        copy_str(s->url, sizeof(s->url), DEFAULTS[i].url);
        copy_str(s->cc, sizeof(s->cc), DEFAULTS[i].cc);
        copy_str(s->codec, sizeof(s->codec), DEFAULTS[i].codec);
        s->kbps = DEFAULTS[i].kbps;
        s->hls = DEFAULTS[i].hls;
        lib->count++;
    }
    return true;
}

void radio_lib_free(radio_lib_t *lib)
{
    free(lib->s);
    memset(lib, 0, sizeof(*lib));
}

int radio_lib_find(const radio_lib_t *lib, const char *url)
{
    if (!url || !url[0]) {
        return -1;
    }
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->s[i].url, url) == 0) {
            return i;
        }
    }
    return -1;
}
