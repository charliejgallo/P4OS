/*
 * CHATARRA - the app
 *
 * The only thing that sees LVGL, the HAL and the file system. The whole game
 * -world, combat, workshop- does not know any of the three exist.
 *
 * Living here:
 *   - the OS's retro canvas, the two layers on it and the camera
 *   - the touch: taps, and two fingers for the zoom
 *   - the USB gamepad, read here and handed to ch_pad.c as plain bits
 *   - the save file
 *
 * ---------------------------------------------------------------------------
 * THE CANVAS (P4OS)
 * ---------------------------------------------------------------------------
 *
 * The watch kept three buffers of its own and blew the frame up x2 by hand.
 * Here the frame IS the OS's retro canvas (docs/RETRO.md): 360x640 standing
 * up, 640x360 lying down, shown x2 so it fills the 720x1280 glass. On it, two
 * layers that never overlap:
 *
 *     the UI      drawn straight onto the canvas, in UI units of 2x2 canvas
 *                 pixels, with its still background in `ubg` (460 KB);
 *     the world   the whole room at the current zoom in `wfb`/`wbg` (up to
 *                 1080x1008, 2.1 MB each) and a WINDOW of it copied onto the
 *                 canvas where the map is seen.
 *
 * The camera is the corner of that window. When the room is smaller than the
 * window it is centred and the rest is a dark margin; when it is bigger, the
 * camera follows the player and stops at the walls. A frame where the camera
 * moved copies the whole window; a frame where it did not copies only the
 * world's dirty rectangles - the watch's rule, one layer further out.
 *
 * All of it through malloc(), which sends anything from 1 KB up to PSRAM.
 *
 * ---------------------------------------------------------------------------
 * THE TOUCH
 * ---------------------------------------------------------------------------
 *
 * Every part of the P4's glass answers, so the menu is a MENU button in the
 * HUD like any other, and the finger is read from the touch panel's own
 * samples (aos_hal_touch_frames(), both fingers) rather than LVGL's pointer:
 *
 *   - one finger that lands, stays put and lifts is a TAP, and it acts on
 *     lifting, where it came down;
 *   - the moment a second finger appears the touch is a PINCH and it can
 *     never be a tap again, however it ends: a zoom never walks the robot
 *     into a wall. The distance between the fingers picks the zoom level.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad.h"
#include "aos_retro.h"
#include "aos_ui.h"

#include "chatarra.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPS         30

#define SAVE_MAGIC  0x43485431u /* "CHT1"                                    */
#define SAVE_VER    5           /* v5: v2's world. Nothing older converts.   */

/* --------------------------------------------------------------------------
 * The v2 format, exactly as it was, so it can be converted
 *
 * It exists because of a bug that reached the board: up to v2 the save file's
 * arrays were sized from the game's enums (`obj[ITEMS]`), so adding an errand
 * item grew an array IN THE MIDDLE of the structure and shifted everything
 * behind it. The loader copied the old save verbatim and read `piezas[]`
 * -full of 0xFF- as if it were quantities: six errands with 255 units.
 *
 * It was fixed by using fixed caps (CH_MAX_OBJ and company). This structure is
 * the EXACT mould of what was written to the cards, and it is converted field
 * by field: nobody loses their game over a bug of mine.
 * -------------------------------------------------------------------------- */
#define V2_ITEMS    17
#define V2_MOCHILA  12
#define V2_PIEZAS   64

typedef struct {
    ch_robot_t yo;
    uint8_t    sala;
    uint8_t    x, y;
    uint8_t    dir;
    uint16_t   creditos;
    uint8_t    obj[V2_ITEMS];
    uint8_t    piezas[V2_MOCHILA];
    uint8_t    bandera[BANDERAS / 8];
    uint16_t   victorias;
    uint32_t   pasos;
    uint8_t    visto[V2_PIEZAS / 8];
} ch_save_v2_t;

/* --------------------------------------------------------------------------
 * v3: everything v4 has, minus the team
 *
 * v4 added `banco[]` and `nbanco` AT THE END, which is the rule the v2 bug
 * wrote. That makes v3 a strict PREFIX of v4 and the conversion a copy of the
 * prefix plus a zeroed tail - no field list, because a field list is a thing
 * that can be got wrong and this is a thing the compiler can check.
 *
 * And it does check it: the assert below is what turns "it is a prefix" from a
 * claim in a comment into a build error the day somebody adds a field in the
 * middle instead of at the end. That is the whole lesson of the x255 bug,
 * written as code rather than as prose.
 * -------------------------------------------------------------------------- */
typedef struct {
    ch_robot_t yo;
    uint8_t    sala;
    uint8_t    x, y;
    uint8_t    dir;
    uint16_t   creditos;
    uint8_t    obj[CH_MAX_OBJ];
    uint8_t    piezas[CH_MAX_MOCHILA];
    uint8_t    bandera[BANDERAS / 8];
    uint16_t   victorias;
    uint32_t   pasos;
    uint8_t    visto[CH_MAX_PIEZAS / 8];
} ch_save_v3_t;

_Static_assert(offsetof(ch_save_t, banco) == sizeof(ch_save_v3_t),
               "v3 dejo de ser un prefijo de v4: el campo nuevo no va en el medio");

typedef struct {
    uint32_t  magic;
    uint16_t  ver;
    uint16_t  largo;
    ch_save_t s;
} sav_t;

typedef struct {
    ch_t        g;

    lv_obj_t   *root;
    const aos_retro_t *r;           /* the OS's canvas: the UI layer's fb    */

    uint16_t   *ubg;                /* the UI layer's still background       */
    uint16_t   *wfbmem, *wbgmem;    /* the world, room-sized at ZOOM_MAX     */
    /* The window as it was when a door at the edge of the map was crossed:
     * the directional transition slides the new room in over it. OPTIONAL:
     * without it every door fades, which is what the game did before. */
    uint16_t   *salmem;
    bool        deslizando;

    /* The map's window on the canvas (canvas pixels) and the camera: the
     * world pixel at its corner, negative when the room is smaller than the
     * window and has margins. */
    int16_t     vx, vy, vw, vh;
    int16_t     camx, camy;
    bool        ventana_toda;       /* copy the whole window this frame      */

    bool        closing;
    bool        saliendo;           /* see chatarra_back()                   */

    /* The finger (see the top of the file). Screen pixels, root-relative. */
    uint32_t    tseq;
    bool        t_on, t_dos, t_mov;
    int16_t     t_x0, t_y0, t_x, t_y;
    uint32_t    t_ms0;
    int16_t     t_d0;               /* the pinch: distance when it started   */
    uint8_t     t_z0;               /* and the zoom it started from          */
    uint8_t     t_n2;               /* samples in a row with two fingers     */
    uint32_t    t_pinza_fin;        /* when the last pinch let go            */

    aos_pad_t   pad;                /* the USB gamepad (ch_pad.c)            */

    uint16_t    frames;
    uint8_t     mostrar_fps;
    uint64_t    prev_ms;
    int16_t     fps;
} app_t;

/* 0 mute, 1 effects only, 2 effects and music. It lives in a PREFERENCE and
 * not in the save file: adding a field to ch_save_t changes its size, and the
 * loader rejects saves whose size does not match. A new option cannot cost
 * anybody their game. */
#define KEY_SND     "ch_snd"
static int s_sonido = 2;

/* --------------------------------------------------------------------------
 * Sound
 *
 * The game calls this without knowing there is a HAL on the other side.
 * aos_hal_beep() enqueues and plays from its own task, so it does not block
 * the drawing.
 * -------------------------------------------------------------------------- */

void ch_sfx(int freq_hz, int ms)
{
    if (s_sonido < 1) return;
    /* With the synthesiser up the effect is a voice of the mix; the beeper is
     * silent anyway while the streaming speaker holds the codec, so this is
     * not a preference, it is the only way it makes a sound at all. */
    if (ch_snd_sintetiza()) {
        ch_snd_sfx(freq_hz, ms);
    } else {
        aos_hal_beep(freq_hz, ms);
    }
}

void ch_tono(int freq_hz, int ms)
{
    if (s_sonido >= 2) {
        aos_hal_beep(freq_hz, ms);
    }
}

int ch_sonido_get(void) { return s_sonido; }

void ch_sonido_set(int v)
{
    s_sonido = v < 0 ? 0 : (v > 2 ? 2 : v);
    aos_hal_pref_set_i32(KEY_SND, s_sonido);
    ch_snd_reabrir();
}

/* --------------------------------------------------------------------------
 * The speaker
 *
 * Five calls, the same shape as the link's eight above: ch_sound.c makes the
 * samples and does not know there is a HAL on the other side.
 * -------------------------------------------------------------------------- */

#ifdef AOS_SIM_BUILTIN
/* CH_WAV=/tmp/x.pcm writes everything the synthesiser produces to a file, raw
 * 16-bit mono. The simulator swallows the samples -its speaker is a stub- so
 * this is the only way to HEAR what was written before it reaches the board.
 * tools/pcm2wav.py puts a header on it. */
static FILE *s_wav;
#endif

bool ch_audio_abrir(int hz)
{
    if (aos_hal_spk_is_open()) return true;
    if (!aos_hal_spk_open((uint32_t)hz)) {
        /* The microphone holds the codec, or this firmware has no streaming
         * speaker. Not a failure: the game falls back to the beeper it was
         * built on, and says so once. */
        aos_hal_log("chatarra", "no speaker: the beeper it is");
        return false;
    }
    aos_hal_log("chatarra", "synthesiser at %d Hz", hz);
    return true;
}

void ch_audio_cerrar(void)
{
#ifdef AOS_SIM_BUILTIN
    if (s_wav) { fclose(s_wav); s_wav = NULL; }
#endif
    aos_hal_spk_close();
}
bool ch_audio_abierto(void)     { return aos_hal_spk_is_open(); }
int  ch_audio_pendiente(void)   { return aos_hal_spk_queued(); }

int ch_audio_escribir(const int16_t *pcm, int n)
{
#ifdef AOS_SIM_BUILTIN
    if (!s_wav) {
        const char *v = getenv("CH_WAV");
        if (v && v[0]) s_wav = fopen(v, "wb");
    }
    if (s_wav) fwrite(pcm, sizeof(int16_t), (size_t)n, s_wav);
#endif
    return aos_hal_spk_write(pcm, n);
}


/* --------------------------------------------------------------------------
 * THE LINK, FROM THIS SIDE OF THE WALL
 *
 * The phone booth in every town talks to the device this one is paired with.
 * The protocol is in ch_link.c and does not know `aos_hal_*` exists: these
 * eight calls are the whole of what it needs, in the same shape as ch_sfx()
 * above. Everything specific to the radio -that a partner lives in NVS, that
 * the host is the lower MAC, that the reliable channel gives up after 3.2 s-
 * stops here.
 *
 * The radio is NOT started when the app opens. A player on the map is off
 * the air: the link costs 4.5 KB of internal RAM and radio time, and the
 * game is played alone nearly always. It goes up when you walk into the booth
 * and comes down when you walk out (and in destroy(), for the way out that
 * skips the door).
 * -------------------------------------------------------------------------- */

#define OFERTA  "Chatarra"      /* what our beacon says, and what we look for */

/* P4OS: the link rides on ESP-NOW through the C6, which the board's HAL does
 * not have yet - aos_hal_link_start() answers false at once. Once it has said
 * so the booth says so too and never tries again in this session: an answer
 * that cannot change is not worth asking twice. */
static bool s_sin_radio;

bool ch_net_hay_pareja(char *nombre, int n)
{
    aos_link_partner_t p;

    if (!aos_hal_link_partner(&p) || !p.valid) return false;
    /* A device whose name was never set answers with an empty one, and an
     * empty subtitle reads as a bug. The booth says "the other one" instead. */
    if (nombre && n > 0) {
        snprintf(nombre, (size_t)n, "%s", p.name[0] ? p.name : _("EL OTRO EQUIPO"));
    }
    return true;
}

bool ch_net_empezar(void)
{
    if (s_sin_radio) return false;
    if (!aos_hal_link_running() && !aos_hal_link_start()) {
        s_sin_radio = true;
        aos_hal_log("chatarra", "no link on this device: the booths stay closed");
        return false;
    }
    aos_hal_link_offer(OFERTA);
    aos_hal_link_reliable_reset();
    return true;
}

void ch_net_parar(void)
{
    if (!aos_hal_link_running()) return;
    aos_hal_link_offer("");
    aos_hal_link_stop();
}

/* The lower MAC is the host. Pong, Truco, the radar and the walkie all decide
 * it the same way and so does this: the rule is only useful if it is the same
 * one everywhere. */
bool ch_net_soy_host(void)
{
    aos_link_stats_t st;
    aos_link_partner_t p;

    if (!aos_hal_link_stats(&st) || !aos_hal_link_partner(&p) || !p.valid) {
        return true;
    }
    return memcmp(st.own_mac, p.mac, 6) < 0;
}

bool ch_net_mandar(const void *d, int n)
{
    return aos_hal_link_send_reliable(d, (size_t)n);
}

int ch_net_recibir(void *d, int max)
{
    aos_link_frame_t f;
    int n = aos_hal_link_recv_reliable(&f);

    if (n <= 0) return 0;
    if (n > max) n = max;
    memcpy(d, f.data, (size_t)n);
    return n;
}

bool ch_net_caido(void)
{
    return aos_hal_link_reliable_lost();
}

void ch_net_reset_canal(void)
{
    aos_hal_link_reliable_reset();
}

/* Their beacon says which app they are offering. Checking it is what lets the
 * booth say "they are not in Chatarra" instead of waiting 3.2 s for a channel
 * that was never going to answer - the trap Pixel Art documented. */
bool ch_net_alla(void)
{
    aos_link_neighbour_t v[AOS_LINK_NEIGHBOURS];
    aos_link_partner_t p;
    int n;

    if (!aos_hal_link_partner(&p) || !p.valid) return false;
    n = aos_hal_link_neighbours(v, AOS_LINK_NEIGHBOURS);
    for (int i = 0; i < n; i++) {
        if (memcmp(v[i].mac, p.mac, 6) == 0) {
            return strcmp(v[i].app, OFERTA) == 0;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * The game
 * -------------------------------------------------------------------------- */

static void ruta_save(char *dst, size_t n)
{
    snprintf(dst, n, "%s/chatarra.sav", aos_hal_path_data());
}

static void nueva_partida(ch_t *g)
{
    memset(&g->s, 0, sizeof(g->s));

    /* The robot you start with: the four most basic parts. It is ugly on
     * purpose, so the first part you tear off somebody shows. */
    for (int c = 0; c < P_CATS; c++) g->s.yo.pieza[c] = 0;
    g->s.yo.skin  = 5;                      /* steel                         */
    g->s.yo.nivel = 5;
    g->s.yo.exp   = ch_exp_nivel(5);
    ch_robot_curar(&g->s.yo);

    for (int i = 0; i < CH_MAX_MOCHILA; i++) g->s.piezas[i] = 0xFF;
    ch_robot_visto(&g->s, &g->s.yo);
    g->s.obj[IT_ACEITE] = 3;
    g->s.creditos = 250;
    g->s.sala = 0;
    g->s.x = 7;                 /* v2: the house is 15x14 now               */
    g->s.y = 10;
    g->s.dir = 1;
}

static bool cargar(ch_t *g)
{
    char ruta[128];
    sav_t sv;
    FILE *f;
    size_t n;

    ruta_save(ruta, sizeof(ruta));
    f = fopen(ruta, "rb");
    if (!f) return false;
    n = fread(&sv, 1, sizeof(sv), f);
    fclose(f);

    if (n < sizeof(sv) - sizeof(ch_save_t) || sv.magic != SAVE_MAGIC) {
        aos_hal_log("chatarra", "unreadable save, discarding it");
        return false;
    }

    memset(&g->s, 0, sizeof(g->s));

    if (sv.ver == SAVE_VER && sv.largo == (uint16_t)sizeof(ch_save_t)) {
        g->s = sv.s;

    } else {
        /* NOTHING OLDER IS CONVERTED, AND THAT IS THE POINT.
         *
         * Up to v4 every version converted the one before it, because the
         * world was the same and only the structure moved. v2 of the GAME
         * redrew the world: the map went from 23x22 cells to 15x14, the rooms
         * were rebuilt and renumbered, and the quest flags index errands that
         * no longer exist. A room number, an x and a y from v1 do not mean
         * anything here.
         *
         * Converting one anyway is how a player ends up standing inside the
         * wall of a house with no way out, which is exactly what happened on
         * the board: (11,14) of the old map clamped to (11,13) of the new one,
         * which is solid. Refusing is the honest answer, and the arrival's
         * search for a free cell -ch_map_entrar()- is the belt to this braces.
         */
        aos_hal_log("chatarra", "save v%u is from the old world: starting fresh",
                    (unsigned)sv.ver);
        return false;
    }

    /* CLEANING UP THE BUG.
     *
     * The games written while the items array was growing in the middle were
     * left with rubbish in the quantities: six errands with 255. An errand's
     * quantity decides nothing -what checks whether you have it is the chest's
     * FLAG, not the counter- so clamping it is safe and leaves the game
     * playable instead of throwing it away. */
    for (int i = 1; i < ITEMS; i++) {
        int tope = (ch_items[i].precio || ch_items[i].combate) ? 99 : 1;
        if (g->s.obj[i] > tope) {
            g->s.obj[i] = (uint8_t)tope;
        }
    }
    for (int i = ITEMS; i < CH_MAX_OBJ; i++) g->s.obj[i] = 0;
    for (int i = MOCHILA; i < CH_MAX_MOCHILA; i++) g->s.piezas[i] = 0xFF;
    if (g->s.sala >= ch_nsalas) g->s.sala = 0;
    if (g->s.nbanco > EQUIPO - 1) g->s.nbanco = EQUIPO - 1;
    ch_robot_stats(&g->s.yo);
    for (int i = 0; i < g->s.nbanco; i++) ch_robot_stats(&g->s.banco[i]);
    return true;
}

static void guardar(ch_t *g)
{
    char ruta[128], tmp[136];
    sav_t sv;
    FILE *f;

    ruta_save(ruta, sizeof(ruta));
    snprintf(tmp, sizeof(tmp), "%s.tmp", ruta);

    sv.magic = SAVE_MAGIC;
    sv.ver   = SAVE_VER;
    sv.largo = (uint16_t)sizeof(ch_save_t);
    sv.s     = g->s;

    /* It is written to a temporary file and renamed: if the power goes
     * halfway, the good save stays where it was. It is the same recipe the
     * portal uses for 'remoto's profile. */
    f = fopen(tmp, "wb");
    if (!f) {
        aos_hal_log("chatarra", "could not write %s", tmp);
        return;
    }
    fwrite(&sv, 1, sizeof(sv), f);
    fclose(f);
    remove(ruta);
    rename(tmp, ruta);
}

/* --------------------------------------------------------------------------
 * The layout
 *
 * Everything the game draws is placed from `ch_lay`, so a change of
 * orientation is this function and a rebuild of the background - the game
 * does not know which way up the screen is.
 * -------------------------------------------------------------------------- */

ch_lay_t ch_lay;

#define KEY_ZOOM_V  "ch_zoom_v"     /* the zoom standing up and lying down:  */
#define KEY_ZOOM_H  "ch_zoom_h"     /* each one has its own natural one      */

static void maquetar(int cw, int chh)
{
    ch_lay_t *l = &ch_lay;

    memset(l, 0, sizeof(*l));
    l->uw = (int16_t)(cw / 2);
    l->uh = (int16_t)(chh / 2);
    l->horiz = (uint8_t)(cw > chh);
    /* The system's home swipe starts in the lowest 36 real pixels: 9 units. */
    l->pie = 9;

    if (l->horiz) {
        /* The HUD is a column on the right; the map takes the rest, which at
         * zoom 2 (the room is 180x168 units: 360x336 canvas pixels) is the
         * whole room with a margin. */
        l->hw = 70;
        l->hh = l->uh;
        l->hx = (int16_t)(l->uw - l->hw);
        l->hy = 0;
        l->sw = l->hx;
        l->sh = l->uh;
        l->dlg_lineas = 4;
    } else {
        /* The HUD is a strip under the map, as on the watch, but a touchable
         * one now: it carries the MENU button. */
        l->hh = 62;
        l->hw = l->uw;
        l->hx = 0;
        l->hy = (int16_t)(l->uh - l->hh);
        l->sw = l->uw;
        l->sh = l->hy;
        l->dlg_lineas = 5;
    }

    /* The dialogue: 27 characters wide, which is what every line in
     * ch_zonas.c was cut to by hand, across the bottom of the map. */
    l->dw = 176;
    l->dh = (int16_t)(10 + l->dlg_lineas * 12);
    l->dx = (int16_t)((l->sw - l->dw) / 2);
    l->dy = (int16_t)(l->sh - l->dh - (l->horiz ? l->pie : 2));
}

/* --------------------------------------------------------------------------
 * The world layer and the camera
 * -------------------------------------------------------------------------- */

#define MARGEN_C    0x07090F        /* around a room smaller than the window */

static int zoom_valido(int z)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    return z - (z - ZOOM_MIN) % ZOOM_PASO;      /* onto a level             */
}

static void mundo_zoom(app_t *a, int z)
{
    ch_t *g = &a->g;

    z = zoom_valido(z);
    g->zoom = (uint8_t)z;
    g->mundo_firma = 0;             /* the room has to be drawn again   */
    ch_buf_escala(&g->wbg, a->wbgmem, WORLD_W, WORLD_H, WORLD_W * z, z);
    ch_buf_escala(&g->wfb, a->wfbmem, WORLD_W, WORLD_H, WORLD_W * z, z);
    ch_dirty_init(&g->wd_prev, WORLD_W, WORLD_H);
    ch_dirty_init(&g->wd_cur,  WORLD_W, WORLD_H);
    ch_dirty_init(&g->wd_push, WORLD_W, WORLD_H);
}

static bool en_mapa(const ch_t *g)
{
    return g->modo == MODO_MAPA || g->modo == MODO_DIALOGO;
}

/* The window: the screen area, minus the dialogue panel when there is one.
 * Canvas pixels. */
static void ventana(app_t *a)
{
    a->vx = 0;
    a->vy = 0;
    a->vw = (int16_t)(SW * 2);
    a->vh = (int16_t)((a->g.modo == MODO_DIALOGO ? ch_lay.dy : SH) * 2);
}

/* Where the camera goes: the player in the middle, stopped at the walls, or
 * the room centred when it fits. */
static void camara(app_t *a, int16_t *cx, int16_t *cy)
{
    const ch_t *g = &a->g;
    int z = g->zoom;
    int rw = WORLD_W * z, rh = WORLD_H * z;
    int px = (g->px + MINI_W / 2) * z, py = (g->py + MINI_H / 2) * z;
    int x, y;

    if (rw <= a->vw) {
        x = -(a->vw - rw) / 2;
    } else {
        x = px - a->vw / 2;
        if (x < 0) x = 0;
        if (x > rw - a->vw) x = rw - a->vw;
    }
    if (rh <= a->vh) {
        y = -(a->vh - rh) / 2;
    } else {
        y = py - a->vh / 2;
        if (y < 0) y = 0;
        if (y > rh - a->vh) y = rh - a->vh;
    }
    *cx = (int16_t)x;
    *cy = (int16_t)y;
}

/* One row of what the window shows: the room where it is, the margin where
 * it is not. `oscuro` is the fade of a doorway, out of sixteen. */
static void fila_ventana(app_t *a, int j, int x0, int x1, uint16_t *dst,
                         int oscuro)
{
    const ch_t *g = &a->g;
    int rw = g->wfb.w, rh = g->wfb.h;
    int wy = a->camy + j;
    uint16_t m = ch_rgb(MARGEN_C);

    for (int i = x0; i < x1; ) {
        int wx = a->camx + i;
        if (wy < 0 || wy >= rh || wx >= rw) {
            dst[i++] = m;
            continue;
        }
        if (wx < 0) {
            int n = -wx;
            if (n > x1 - i) n = x1 - i;
            for (int k = 0; k < n; k++) dst[i + k] = m;
            i += n;
            continue;
        }
        int n = rw - wx;
        if (n > x1 - i) n = x1 - i;
        const uint16_t *src = g->wfb.px + (size_t)wy * g->wfb.stride + wx;
        if (oscuro) {
            for (int k = 0; k < n; k++) dst[i + k] = ch_mix(src[k], 0, oscuro);
        } else {
            memcpy(dst + i, src, (size_t)n * sizeof(uint16_t));
        }
        i += n;
    }
}

static void copiar_ventana(app_t *a, int x0, int y0, int x1, int y1, int oscuro)
{
    const aos_retro_t *r = a->r;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > a->vw) x1 = a->vw;
    if (y1 > a->vh) y1 = a->vh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) {
        uint16_t *dst = r->px + (size_t)(a->vy + j) * r->w + a->vx;
        fila_ventana(a, j, x0, x1, dst, oscuro);
    }
    aos_retro_present_rect(a->vx + x0, a->vy + y0, x1 - x0, y1 - y0);
}

/* The world's dirty rectangles, from world units to the window. */
static void copiar_sucios(app_t *a, const ch_dirty_t *d)
{
    int z = a->g.zoom;

    for (int i = 0; i < d->n; i++) {
        const ch_rect_t *q = &d->r[i];
        copiar_ventana(a, q->x0 * z - a->camx, q->y0 * z - a->camy,
                       q->x1 * z - a->camx, q->y1 * z - a->camy, 0);
    }
}

/* THE DIRECTIONAL TRANSITION
 *
 * A door at the edge of the map is a step to the next screen of the same
 * place, so the new room comes in FROM THE SIDE YOU WALKED TOWARDS and pushes
 * the old one out. A door in the middle of a room is a doorway into somewhere
 * else and keeps the fade: walking into a house is not walking east.
 *
 * Nothing moves during those nine frames, so the slide is a copy: the window
 * as it was (salmem) and the new room as the camera sees it. */
static void mezclar(app_t *a, int p)
{
    ch_t *g = &a->g;
    const aos_retro_t *r = a->r;
    int horiz = (g->trans_dir >= 2);
    int largo = horiz ? a->vw : a->vh;
    int falta = largo * (TRANS_N - p) / TRANS_N;
    int signo = (g->trans_dir == 1 || g->trans_dir == 2) ? -1 : +1;
    int dn = signo * falta;             /* the new room's offset             */
    int dv = dn - signo * largo;        /* the old one's, one screen behind  */

    for (int j = 0; j < a->vh; j++) {
        uint16_t *dst = r->px + (size_t)(a->vy + j) * r->w + a->vx;
        if (horiz) {
            /* the new room covers columns [dn, dn + vw) of the window */
            int n0 = dn < 0 ? 0 : dn, n1 = dn + a->vw > a->vw ? a->vw : dn + a->vw;
            for (int i = 0; i < a->vw; i++) {
                if (i >= n0 && i < n1) continue;
                int sx = i - dv;
                dst[i] = (sx >= 0 && sx < a->vw) ? a->salmem[(size_t)j * a->vw + sx] : 0;
            }
            if (n1 > n0) {
                /* the new room's row, shifted: build it in place */
                int save_cx = a->camx;
                a->camx = (int16_t)(a->camx - dn);
                fila_ventana(a, j, n0, n1, dst, 0);
                a->camx = (int16_t)save_cx;
            }
        } else {
            int sy = j - dn;
            if (sy >= 0 && sy < a->vh) {
                int save_cy = a->camy;
                a->camy = (int16_t)(a->camy - dn);
                fila_ventana(a, j, 0, a->vw, dst, 0);
                a->camy = (int16_t)save_cy;
            } else {
                int oy = j - dv;
                if (oy >= 0 && oy < a->vh) {
                    memcpy(dst, a->salmem + (size_t)oy * a->vw,
                           (size_t)a->vw * sizeof(uint16_t));
                } else {
                    memset(dst, 0, (size_t)a->vw * sizeof(uint16_t));
                }
            }
        }
    }
    aos_retro_present_rect(a->vx, a->vy, a->vw, a->vh);
}

/* --------------------------------------------------------------------------
 * Presenting
 * -------------------------------------------------------------------------- */

static void presentar_ui(app_t *a, const ch_dirty_t *d)
{
    if (d->all) {
        aos_retro_present();
        return;
    }
    for (int i = 0; i < d->n; i++) {
        const ch_rect_t *q = &d->r[i];
        aos_retro_present_rect(q->x0 * 2, q->y0 * 2, (q->x1 - q->x0) * 2,
                               (q->y1 - q->y0) * 2);
    }
}

/* Does a rectangle of the UI (units) touch the map's window? If the UI
 * restored something there, the world has to be copied whole on top. */
static bool pisa_ventana(const app_t *a, const ch_dirty_t *d)
{
    if (d->all) return true;
    for (int i = 0; i < d->n; i++) {
        const ch_rect_t *q = &d->r[i];
        if (q->x0 * 2 < a->vx + a->vw && q->x1 * 2 > a->vx &&
            q->y0 * 2 < a->vy + a->vh && q->y1 * 2 > a->vy) return true;
    }
    return false;
}

static void dibujar_ui(ch_t *g)
{
    if (g->modo == MODO_COMBATE) {
        ch_bt_dibujar(g);
    } else if (g->modo == MODO_FERIA) {
        ch_fe_dibujar(g);
    } else {
        ch_ui_dibujar(g);
    }
    ch_pad_dibujar(g);              /* the pad's cursor, over all of it  */
}

/* Rebuilds every background for the current mode. It is the only expensive
 * frame and it happens on changing room, menu, zoom or combat phase: never
 * continuously. */
static void rehacer(app_t *a)
{
    ch_t *g = &a->g;
    const aos_retro_t *r = a->r;
    bool mapa = en_mapa(g);

    /* The window that is LEAVING has to be kept before anything is drawn
     * over it: at this point the canvas still shows it. */
    a->deslizando = mapa && g->trans && g->trans_dir != 0xFF && a->salmem &&
                    a->vw > 0 && a->vh > 0;
    if (a->deslizando) {
        for (int j = 0; j < a->vh; j++) {
            memcpy(a->salmem + (size_t)j * a->vw,
                   r->px + (size_t)(a->vy + j) * r->w + a->vx,
                   (size_t)a->vw * sizeof(uint16_t));
        }
    }

    /* Whatever a screen does not paint must not be the last one's: after a
     * turn of the screen the background holds the other orientation. */
    memset(a->ubg, 0, (size_t)r->w * r->h * sizeof(uint16_t));
    if (g->modo == MODO_COMBATE) {
        ch_bt_fondo(g);
    } else {
        ch_ui_fondo(g);
        if (g->modo != MODO_TITULO) {
            ch_ui_hud(g);
        }
    }

    memcpy(r->px, a->ubg, (size_t)r->w * r->h * sizeof(uint16_t));
    ch_dirty_reset(&g->d_prev);
    ch_dirty_reset(&g->d_cur);
    dibujar_ui(g);

    ventana(a);
    if (mapa) {
        memcpy(g->wfb.px, g->wbg.px,
               (size_t)g->wbg.stride * g->wbg.h * sizeof(uint16_t));
        ch_dirty_reset(&g->wd_prev);
        ch_dirty_reset(&g->wd_cur);
        ch_map_dibujar(g);
        camara(a, &a->camx, &a->camy);
        if (a->deslizando) {
            mezclar(a, 0);
        } else {
            copiar_ventana(a, 0, 0, a->vw, a->vh,
                           g->trans && g->trans_dir == 0xFF ? g->trans * 2 : 0);
        }
        ch_dirty_all(&g->wd_prev);
    }

    aos_retro_present();
    ch_dirty_all(&g->d_prev);
    g->d_cur = g->d_prev;
    g->rehacer_fondo = 0;
    g->hud_sucio = 0;
    a->ventana_toda = true;
}

static void present(app_t *a)
{
    ch_t *g = &a->g;

    if (g->rehacer_fondo) {
        rehacer(a);
        return;
    }

    /* --- the UI layer: on the canvas itself ---------------------------- */
    g->d_push = g->d_prev;
    if (g->d_push.all) {
        memcpy(g->fb.px, g->bg.px, (size_t)g->fb.stride * g->fb.h * sizeof(uint16_t));
    } else {
        for (int i = 0; i < g->d_push.n; i++) {
            ch_restore(&g->fb, &g->bg, &g->d_push.r[i]);
        }
    }
    ch_dirty_reset(&g->d_cur);
    dibujar_ui(g);
    /* the HUD, only when a number changed */
    if (g->hud_sucio && g->modo != MODO_TITULO && g->modo != MODO_COMBATE) {
        ch_ui_hud(g);
    }
    ch_dirty_join(&g->d_push, &g->d_cur);
    presentar_ui(a, &g->d_push);
    g->d_prev = g->d_cur;

    if (!en_mapa(g)) return;

    /* --- the world layer: through the window --------------------------- */
    ventana(a);
    if (pisa_ventana(a, &g->d_push)) a->ventana_toda = true;

    if (a->deslizando) {
        mezclar(a, TRANS_N - (int)g->trans);
        if (!g->trans) {
            /* Arrived. The next frame restores everything, which is what
             * wd_prev being whole means. */
            a->deslizando = false;
            ch_dirty_all(&g->wd_prev);
            a->ventana_toda = true;
        }
        return;
    }

    g->wd_push = g->wd_prev;
    if (g->wd_push.all) {
        memcpy(g->wfb.px, g->wbg.px,
               (size_t)g->wbg.stride * g->wbg.h * sizeof(uint16_t));
    } else {
        for (int i = 0; i < g->wd_push.n; i++) {
            ch_restore(&g->wfb, &g->wbg, &g->wd_push.r[i]);
        }
    }
    ch_dirty_reset(&g->wd_cur);
    ch_map_dibujar(g);
    ch_dirty_join(&g->wd_push, &g->wd_cur);

    {
        int16_t cx, cy;
        bool fundido = g->trans && g->trans_dir == 0xFF;
        camara(a, &cx, &cy);
        if (cx != a->camx || cy != a->camy || fundido || g->wd_push.all) {
            a->ventana_toda = true;
        }
        a->camx = cx;
        a->camy = cy;
        if (a->ventana_toda) {
            copiar_ventana(a, 0, 0, a->vw, a->vh, fundido ? g->trans * 2 : 0);
            /* the frame after a fade still has to clear its last shade */
            a->ventana_toda = fundido;
        } else {
            copiar_sucios(a, &g->wd_push);
        }
    }
    g->wd_prev = g->wd_cur;
}

/* --------------------------------------------------------------------------
 * The zoom
 * -------------------------------------------------------------------------- */

static void guardar_zoom(const app_t *a)
{
    aos_hal_pref_set_i32(HORIZ ? KEY_ZOOM_H : KEY_ZOOM_V, a->g.zoom);
}

static void cambiar_zoom(app_t *a, int z)
{
    z = zoom_valido(z);
    if (z == a->g.zoom) return;
    mundo_zoom(a, z);
    guardar_zoom(a);
    /* The room is drawn again at the new size, and the camera finds the
     * player on it: the zoom always recentres on your robot. */
    a->g.rehacer_fondo = 1;
    ch_sfx(700 + z * 150, 25);
}

/* The natural zoom of each orientation: the biggest at which the room's whole
 * HEIGHT fits the window, so the camera only ever slides one way. */
static int zoom_natural(void)
{
    for (int z = ZOOM_MAX; z > ZOOM_MIN; z -= ZOOM_PASO) {
        if (WORLD_H * z <= SH * 2) return z;
    }
    return ZOOM_MIN;
}

/* --------------------------------------------------------------------------
 * The finger
 * -------------------------------------------------------------------------- */

#define TAP_SLOP    28          /* screen px a tap may wander                */
#define TAP_MAX_MS  1200        /* a press longer than this is not a tap     */

static void tocar(app_t *a, int sx, int sy)
{
    ch_t *g = &a->g;
    const aos_retro_t *r = a->r;
    int cx = (sx - r->x) / r->scale, cy = (sy - r->y) / r->scale;

    if (cx < 0 || cy < 0 || cx >= r->w || cy >= r->h) return;
    g->pad_visto = 0;               /* a finger: the pad's cursor goes   */

    /* On the map, the window is the world and the rest is the HUD. */
    if (g->modo == MODO_MAPA && cx >= a->vx && cx < a->vx + a->vw &&
        cy >= a->vy && cy < a->vy + a->vh) {
        int wx = a->camx + (cx - a->vx), wy = a->camy + (cy - a->vy);
        if (wx < 0 || wy < 0) return;
        wx /= g->zoom;
        wy /= g->zoom;
        if (wx >= WORLD_W || wy >= WORLD_H) return;     /* the margin    */
#ifdef AOS_SIM_BUILTIN
        if (getenv("CH_TOQUES")) {
            aos_hal_log("chatarra", "tap %d,%d -> cell %d,%d (player %d,%d)",
                        sx, sy, wx / TILE, wy / TILE, g->s.x, g->s.y);
        }
#endif
        ch_map_toque(g, wx, wy);
        return;
    }
    ch_ui_toque(g, cx / 2, cy / 2);
}

static int distancia(const aos_touch_frame_t *f)
{
    int dx = f->x[0] - f->x[1], dy = f->y[0] - f->y[1];
    return ch_isqrt(dx * dx + dy * dy);
}

/* The zoom the pinch asks for: a level per third of the distance, from the
 * zoom it started at. Whole levels only - the art is pixels at twice the
 * world's resolution, so an odd zoom would give an art pixel half a canvas
 * pixel too many and smear every tile. */
static int zoom_de_pinza(const app_t *a, int d)
{
    int z = a->t_z0;
    int d0 = a->t_d0 > 40 ? a->t_d0 : 40;

    if (d * 100 >= d0 * 175)      z += 2 * ZOOM_PASO;
    else if (d * 100 >= d0 * 130) z += ZOOM_PASO;
    else if (d * 100 <= d0 * 55)  z -= 2 * ZOOM_PASO;
    else if (d * 100 <= d0 * 77)  z -= ZOOM_PASO;
    return z;
}

static void leer_dedos(app_t *a)
{
    aos_touch_frame_t fr[16];
    uint32_t n = aos_hal_touch_frames(a->tseq, fr, 16);
    lv_area_t rc;

    lv_obj_get_coords(a->root, &rc);        /* the runtime slides the root */

    for (uint32_t k = 0; k < n; k++) {
        const aos_touch_frame_t *f = &fr[k];
        a->tseq = f->seq;

        if (f->count == 0) {
            if (a->t_on && !a->t_dos && !a->t_mov &&
                f->t_ms - a->t_ms0 <= TAP_MAX_MS) {
                tocar(a, a->t_x0, a->t_y0);
            }
            if (a->t_on && a->t_dos) a->t_pinza_fin = f->t_ms;
            a->t_on = false;
            a->t_n2 = 0;
            continue;
        }

        int x = f->x[0] - rc.x1, y = f->y[0] - rc.y1;
        if (!a->t_on) {
            a->t_on = true;
            a->t_mov = false;
            /* Two fingers never lift at exactly the same sample: the one
             * left behind a moment after a pinch - or the chip losing both
             * for a sample and finding one again - is still the pinch. */
            a->t_dos = a->t_pinza_fin && f->t_ms - a->t_pinza_fin < 300;
            a->t_x0 = a->t_x = (int16_t)x;
            a->t_y0 = a->t_y = (int16_t)y;
            a->t_ms0 = f->t_ms;
            a->t_n2 = 0;
        }

        if (f->count >= 2) {
            /* A second finger: from here on this touch is a pinch, and a
             * pinch is never a tap. The zoom follows only on the map; two
             * samples in a row, because the first one can be the chip's. */
            a->t_dos = true;
            if (a->t_n2 < 255) a->t_n2++;
            if (a->t_n2 == 2) {
                a->t_d0 = (int16_t)distancia(f);
                a->t_z0 = a->g.zoom;
            } else if (a->t_n2 > 2 && en_mapa(&a->g)) {
                int z = zoom_de_pinza(a, distancia(f));
                if (z < ZOOM_MIN) z = ZOOM_MIN;
                if (z > ZOOM_MAX) z = ZOOM_MAX;
                if (z != a->g.zoom) cambiar_zoom(a, z);
            }
            continue;
        }
        a->t_n2 = 0;
        a->t_x = (int16_t)x;
        a->t_y = (int16_t)y;
        if (x - a->t_x0 > TAP_SLOP || a->t_x0 - x > TAP_SLOP ||
            y - a->t_y0 > TAP_SLOP || a->t_y0 - y > TAP_SLOP) {
            a->t_mov = true;
        }
    }
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * Read once a frame and handed over as bits: the game's own enum is the same
 * as aos_pad.h's, so it is a cast, and the asserts are what keep it one.
 * -------------------------------------------------------------------------- */

_Static_assert((int)CHP_ARRIBA == (int)AOS_PAD_UP &&
               (int)CHP_ABAJO == (int)AOS_PAD_DOWN &&
               (int)CHP_IZQ == (int)AOS_PAD_LEFT &&
               (int)CHP_DER == (int)AOS_PAD_RIGHT &&
               (int)CHP_A == (int)AOS_PAD_A && (int)CHP_B == (int)AOS_PAD_B &&
               (int)CHP_L == (int)AOS_PAD_L && (int)CHP_R == (int)AOS_PAD_R &&
               (int)CHP_START == (int)AOS_PAD_START,
               "the game's pad bits are aos_pad.h's");

static void leer_pad(app_t *a)
{
    aos_pad_update(&a->pad, lv_tick_get());
    ch_pad(&a->g, a->pad.held, a->pad.pressed, a->pad.repeat);
}

/* --------------------------------------------------------------------------
 * The game's clock
 * -------------------------------------------------------------------------- */

static void step(void *user)
{
    app_t *a = (app_t *)user;
    ch_t  *g = &a->g;

    if (a->closing) return;

    /* Leaving has to be deferred: aos_ui_back() destroys the app, so calling
     * it from inside the game would be destroying it while it runs. */
    if (g->quiere_salir) {
        g->quiere_salir = 0;
        a->saliendo = true;         /* so back() lets the call through       */
        guardar(g);
        aos_ui_back();
        return;
    }
    if (g->quiere_guardar) {
        g->quiere_guardar = 0;
        guardar(g);
    }

    leer_dedos(a);
    leer_pad(a);
    if (g->zoom_pide) {
        cambiar_zoom(a, g->zoom + g->zoom_pide * ZOOM_PASO);
        g->zoom_pide = 0;
    }

    if (g->aviso_t && --g->aviso_t == 0) g->hud_sucio = 1;
    ch_snd_tick(g);

    switch (g->modo) {
    case MODO_MAPA:
    case MODO_DIALOGO:
        ch_map_tick(g);
        break;
    case MODO_COMBATE:
        ch_bt_tick(g);
        /* A link battle still has to drain the channel: the rival's choice
         * comes in through it, and so does the news that they walked out. */
        if (g->bt.enlace) ch_lk_tick(g);
        break;
    case MODO_CABINA:
        ch_lk_tick(g);
        g->cuadro++;
        break;
    case MODO_FERIA:
        ch_fe_tick(g);
        break;
    default:
        g->cuadro++;
        break;
    }

    present(a);

    if (a->mostrar_fps && ++a->frames >= 30) {
        uint64_t ahora = aos_hal_uptime_ms();
        uint32_t dt = (uint32_t)(ahora - a->prev_ms);
        a->prev_ms = ahora;
        a->fps = (int16_t)(dt ? (int)(a->frames * 1000u / dt) : 0);
        aos_hal_log("chatarra", "%d fps, %d %% of the UI, %d %% of the world",
                    a->fps, ch_dirty_area(&g->d_push) * 100 / (UW * UH),
                    ch_dirty_area(&g->wd_push) * 100 / (WORLD_W * WORLD_H));
        a->frames = 0;
    }
}

static bool chatarra_back(aos_app_t *self, void *inst)
{
    app_t *a = (app_t *)inst;

    (void)self;

    /* THE TRAP: aos_ui_back() asks THIS callback first. Since leaving has to
     * be deferred -it destroys the app-, the tick ends up calling
     * aos_ui_back()... which re-enters here, asks to leave again and NEVER
     * closes. From outside it looks like an app you cannot leave, and the
     * loop gives no error at all. The flag is what breaks the cycle. */
    if (!a || a->saliendo) return false;

    if (ch_ui_atras(&a->g)) return true;
    a->g.quiere_salir = 1;
    return true;
}

/* --------------------------------------------------------------------------
 * The canvas, for this orientation
 * -------------------------------------------------------------------------- */

static bool armar_lienzo(app_t *a, lv_obj_t *root)
{
    ch_t *g = &a->g;
    int w, h;

    lv_obj_update_layout(root);
    w = (int)lv_obj_get_width(root) / 2;
    h = (int)lv_obj_get_height(root) / 2;
    /* The canvas is the root in halves, x2: the whole glass, no controls of
     * the OS - the buttons are the game's, drawn in its own style. */
    a->r = aos_retro_begin(root, w, h, 2, AOS_RETRO_CENTER);
    if (!a->r) return false;
    aos_retro_set_border(MARGEN_C);

    maquetar(a->r->w, a->r->h);
    ch_buf_escala(&g->fb, a->r->px, UW, UH, a->r->w, 2);
    ch_buf_escala(&g->bg, a->ubg,   UW, UH, a->r->w, 2);
    ch_dirty_init(&g->d_prev, UW, UH);
    ch_dirty_init(&g->d_cur,  UW, UH);
    ch_dirty_init(&g->d_push, UW, UH);
    return true;
}

static void zoom_de_orientacion(app_t *a)
{
    int32_t z = 0;

    if (!aos_hal_pref_get_i32(HORIZ ? KEY_ZOOM_H : KEY_ZOOM_V, &z) ||
        z < ZOOM_MIN || z > ZOOM_MAX || (z - ZOOM_MIN) % ZOOM_PASO) {
        z = zoom_natural();
    }
    mundo_zoom(a, (int)z);
}

/* The finger that opened the app (or turned the screen) is no tap. */
static void dedos_desde_ahora(app_t *a)
{
    aos_touch_frame_t now;

    a->t_on = false;
    if (aos_hal_touch_frame(&now)) {
        a->tseq = now.seq;
        /* still down: it counts as a touch that already moved */
        if (now.count) {
            a->t_on = a->t_mov = true;
            a->t_dos = now.count > 1;
        }
    }
}

static bool chatarra_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    app_t *a = (app_t *)inst;

    (void)self;
    if (!a) return false;
    aos_retro_stop();
    aos_retro_end();
    if (!armar_lienzo(a, root)) {
        aos_hal_log("chatarra", "no canvas for the new orientation");
        return false;
    }
    zoom_de_orientacion(a);
    a->deslizando = false;
    a->g.rehacer_fondo = 1;
    dedos_desde_ahora(a);
    aos_pad_reset(&a->pad, lv_tick_get());
    aos_retro_run(FPS, step, NULL, a);
    return true;
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void liberar(app_t *a)
{
    free(a->ubg);
    free(a->wfbmem);
    free(a->wbgmem);
    free(a->salmem);
    lv_free(a);
}

static void *chatarra_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    size_t mundo = (size_t)WORLD_W * ZOOM_MAX * WORLD_H * ZOOM_MAX * sizeof(uint16_t);
    size_t lienzo;

    (void)self;
    if (!a) return NULL;

    uint32_t hint = 0, hpsram = 0;
    aos_hal_heap_info(&hint, &hpsram);
    aos_hal_log("chatarra", "opening | internal %u B, psram %u B",
                (unsigned)hint, (unsigned)hpsram);

    a->root = root;
    lv_obj_update_layout(root);
    lienzo = (size_t)(lv_obj_get_width(root) / 2) * (size_t)(lv_obj_get_height(root) / 2)
             * sizeof(uint16_t);
    a->ubg    = (uint16_t *)malloc(lienzo);
    a->wfbmem = (uint16_t *)malloc(mundo);
    a->wbgmem = (uint16_t *)malloc(mundo);
    if (!a->ubg || !a->wfbmem || !a->wbgmem) {
        aos_hal_log("chatarra", "out of memory for the buffers");
        liberar(a);
        return NULL;
    }
    memset(a->ubg, 0, lienzo);
    memset(a->wfbmem, 0, mundo);
    memset(a->wbgmem, 0, mundo);
    /* The still window of a directional transition. OPTIONAL: if it does not
     * fit, trans_dir is never honoured and every door fades. A nicety is not
     * worth failing to open over. */
    a->salmem = (uint16_t *)malloc(lienzo);
    if (!a->salmem) aos_hal_log("chatarra", "no room for the slide: the doors will fade");

    if (!armar_lienzo(a, root)) {
        aos_hal_log("chatarra", "no canvas");
        liberar(a);
        return NULL;
    }
    zoom_de_orientacion(a);

    ch_pal_init();
    a->g.rng = (uint32_t)aos_hal_uptime_ms() | 1u;

    if (!cargar(&a->g)) {
        nueva_partida(&a->g);
    }
    {
        int32_t v = 2;
        if (!aos_hal_pref_get_i32(KEY_SND, &v) || v < 0 || v > 2) v = 2;
        s_sonido = (int)v;
    }
    a->g.modo = MODO_TITULO;
    a->g.rehacer_fondo = 1;
    ventana(a);
#ifdef AOS_SIM_BUILTIN
    /* Development switches. They only exist in the simulator: on the board
     * getenv() always returns NULL.
     *
     *   CH_SALA=5     starts straight in that room
     *   CH_NIVEL=20   the robot's level
     *   CH_PIEZAS=1   the bag full, for testing the workshop
     *   CH_COMBATE=1  opens straight into a fight
     *   CH_FPS=1      frames per second and % of screen pushed
     *   CH_MUDO=1     no beeps
     *   CH_CHECK=1    checks the doors and decorations of EVERY room
     *   CH_FINAL=1    opens the closing screen straight away
     *   CH_MENU=0|1|2 opens the menu at that page (root, yours, the game)
     *   CH_ZOOM=2|4|6 the world's zoom, as if pinched
     *   CH_MODO=<n>   opens straight into a screen (see the MODO_ enum)
     *   CH_CABINA=99  the booth for real: ch_lk_entrar(), radio and all
     *   CH_CABINA=<n> the booth, at that state (see the LK_ enum): the menu
     *                 needs a second watch, and its layout does not.
     *   CH_FERIA=1    la cinta de chatarra, directo
     *   CH_FERIA=<n>  con n>1, termina ya con n piezas: es la unica forma de
     *                 ver la pantalla de premio sin jugar cuarenta segundos
     *   CH_HORA=<0..23> la hora del mundo: el dia entero son dos horas de
     *                 juego, asi que verlo sin esto es esperar

     */
    {
        const char *v;
        if ((v = getenv("CH_MUDO")) && v[0]) s_sonido = 0;
        if ((v = getenv("CH_CHECK")) && v[0]) ch_map_check();
        if ((v = getenv("CH_FINAL")) && v[0]) { a->g.modo = MODO_FINAL; }
        /* CH_MKV2=1 writes a save in the OLD format, with the bag full of
         * 0xFF, which is exactly what is on the cards. It is for checking the
         * conversion without the board: run once with the variable set and
         * once without. */
        if ((v = getenv("CH_MKV2")) && v[0]) {
            struct { uint32_t magic; uint16_t ver; uint16_t largo;
                     ch_save_v2_t s; } vv;
            char ruta[128];
            FILE *f;
            memset(&vv, 0, sizeof(vv));
            vv.magic = SAVE_MAGIC; vv.ver = 2;
            vv.largo = (uint16_t)sizeof(ch_save_v2_t);
            vv.s.yo = a->g.s.yo;
            vv.s.sala = 4; vv.s.x = 7; vv.s.y = 10; vv.s.creditos = 1234;
            vv.s.victorias = 7; vv.s.pasos = 999;
            vv.s.obj[IT_ACEITE] = 3;
            for (int i = 0; i < V2_MOCHILA; i++) vv.s.piezas[i] = 0xFF;
            vv.s.piezas[0] = 20;
            vv.s.bandera[0] = 0x06;
            ruta_save(ruta, sizeof(ruta));
            f = fopen(ruta, "wb");
            if (f) { fwrite(&vv, 1, sizeof(vv), f); fclose(f); }
            aos_hal_log("chatarra", "test v2 save written (%u B)",
                        (unsigned)sizeof(vv));
        }
        if ((v = getenv("CH_FPS")) && v[0])  a->mostrar_fps = 1;
        if ((v = getenv("CH_ZOOM")) && v[0]) mundo_zoom(a, atoi(v));
        if ((v = getenv("CH_NIVEL")) && v[0]) {
            a->g.s.yo.nivel = (uint8_t)atoi(v);
            a->g.s.yo.exp = ch_exp_nivel(a->g.s.yo.nivel);
            ch_robot_curar(&a->g.s.yo);
        }
        if ((v = getenv("CH_PIEZAS")) && v[0]) {
            uint32_t sem = 7u;
            /* Besides filling the bag, it fits a set of parts with attacks of
             * SEVERAL types: without this the starting robot only hits with
             * impact and there is no way to see the ranged animations. */
            a->g.s.yo.pieza[P_CABEZA]  = 6;      /* Cyclops: fire           */
            a->g.s.yo.pieza[P_TORSO]   = 3;      /* Reactor: plasma         */
            a->g.s.yo.pieza[P_BRAZOS]  = 12;     /* Lances: cryo            */
            a->g.s.yo.pieza[P_PIERNAS] = 10;     /* Unicycle: volt          */
            ch_robot_curar(&a->g.s.yo);
            for (int i = 0; i < MOCHILA; i++) {
                a->g.s.piezas[i] = (uint8_t)ch_rnd(&sem, PIEZAS);
            }
            for (int i = 1; i < ITEMS; i++) a->g.s.obj[i] = 5;
            a->g.s.creditos = 4000;
            /* and a full team, which is the only way to see the swap button
             * lit up without playing for an hour first */
            ch_eq_armar(&a->g.s);
            ch_eq_armar(&a->g.s);
        }
        if ((v = getenv("CH_MEL")) && v[0]) {
            /* Forces a tune, to listen to it without playing up to it. */
            ch_snd_melodia(&a->g, atoi(v));
        }
        if ((v = getenv("CH_HORA")) && v[0]) {
            int h = atoi(v) % 24;
            a->g.s.pasos = (uint32_t)(((h + 24 - 8) % 24) * 300);
            a->g.rehacer_fondo = 1;
        }
        if ((v = getenv("CH_SALA")) && v[0]) {
            a->g.modo = MODO_MAPA;
            ch_map_entrar(&a->g, atoi(v), 11, 14);
        }
        if ((v = getenv("CH_MODO")) && v[0]) {
            a->g.modo_prev = MODO_MAPA;
            a->g.modo = (uint8_t)atoi(v);
            a->g.rehacer_fondo = 1;
        }
        if ((v = getenv("CH_FERIA")) && v[0]) {
            int n = atoi(v);
            ch_fe_entrar(&a->g);
            if (n > 1) { a->g.fe.puntos = (uint8_t)n; a->g.fe.resta = 1; }
        }
        if ((v = getenv("CH_CABINA")) && atoi(v) == 99) {
            /* 99: walk in for real, radio and all - the closed door */
            ch_lk_entrar(&a->g);
        } else if ((v = getenv("CH_CABINA")) && v[0]) {
            a->g.modo = MODO_CABINA;
            a->g.lk.estado = (uint8_t)atoi(v);
            snprintf(a->g.lk.nombre, sizeof(a->g.lk.nombre), "RELOJ 2");
            snprintf(a->g.lk.linea[0], sizeof(a->g.lk.linea[0]),
                     "ENLACE ABIERTO.");
            snprintf(a->g.lk.linea[1], sizeof(a->g.lk.linea[1]),
                     "DEL OTRO LADO: RELOJ 2.");
            a->g.rehacer_fondo = 1;
        }
        if ((v = getenv("CH_MENU")) && v[0]) {
            /* Straight into a page of the menu, without the HUD's button. */
            ch_ui_menu(&a->g);
            a->g.sel2 = (uint8_t)atoi(v);
        }
        if ((v = getenv("CH_COMBATE")) && v[0]) {
            ch_robot_t rival;
            uint32_t sem = 12345u;
            /* The value is the ZONE, which is what picks the arena: eight
             * arenas cannot be looked at one by one without this. */
            int z = atoi(v);
            if (z < 1 || z > ZONAS) z = 1;
            a->g.modo = MODO_MAPA;
            ch_map_entrar(&a->g, 4, 7, 10);
            ch_robot_random(&rival, &sem, a->g.s.yo.nivel + 1, (uint8_t)z);
            ch_bt_empezar(&a->g, &rival, 0, z);
            /* CH_HERIDO leaves both of them under a quarter: it is the only
             * way to look at the sparks without losing a fight first. */
            if ((v = getenv("CH_HERIDO")) && v[0]) {
                a->g.s.yo.vida      = (int16_t)(a->g.s.yo.vida_max / 6);
                a->g.bt.rival.vida  = (int16_t)(a->g.bt.rival.vida_max / 6);
                a->g.bt.hp_ver[0]   = a->g.s.yo.vida;
                a->g.bt.hp_ver[1]   = a->g.bt.rival.vida;
            }
        }
    }
#endif

    ch_snd_init();

    a->prev_ms = aos_hal_uptime_ms();
    dedos_desde_ahora(a);
    aos_pad_reset(&a->pad, lv_tick_get());
    aos_retro_run(FPS, step, NULL, a);
    aos_hal_log("chatarra", "ready | canvas %dx%d x%d, UI %dx%d, zoom %d",
                a->r->w, a->r->h, a->r->scale, UW, UH, a->g.zoom);
    return a;
}

static void chatarra_destroy(aos_app_t *self, void *inst)
{
    app_t *a = (app_t *)inst;

    (void)self;
    if (!a) return;
    a->closing = true;

    aos_retro_stop();
    guardar(&a->g);
    ch_snd_fin();               /* the codec goes back to whoever wants it */
    ch_net_parar();             /* the way out that skips the booth's door */
    aos_retro_end();            /* frees the canvas: nothing draws after it */
    liberar(a);
}

/* --------------------------------------------------------------------------
 * The icon, inside the .so
 *
 * There was no robot among the firmware's icons, so the game brings its own
 * head: the AIC format (docs/ICONS.md) is the drawing written as bytes, the
 * app hands the blob over in init() and the launcher interprets it.
 *
 * Every number is a percent of the icon size, which is why one blob serves
 * every size the launcher draws. `icon_vec` stays as a fallback: a firmware
 * without the call shows a gamepad, which beats showing nothing.
 * -------------------------------------------------------------------------- */
static const uint8_t CHATARRA_ICON[] = {
    AIC_HEADER,
    /* the aerial, and its little red lamp */
    AIC_RECT(AIC_CENTER,   0, -34,  4, 20, 0,          AIC_C_DIM, 255),
    AIC_RECT(AIC_CENTER,   0, -46, 11, 11, AIC_CIRCLE, AIC_C_RED, 255),
    /* the shoulders, behind the head */
    AIC_RECT(AIC_CENTER,   0,  28, 52, 20, 6,  AIC_C_LIT(0x8E4630), 255),
    /* the head */
    AIC_RECT(AIC_CENTER,   0,  -2, 56, 46, 10, AIC_C_LIT(0xC8763F), 255),
    AIC_BORDER(AIC_DIV(26),                    AIC_C_LIT(0x5A2E17), 255),
    AIC_INTO,
        AIC_RECT(AIC_CENTER,    -13, -6, 14, 14, 3, AIC_C_YELLOW,        255),
        AIC_RECT(AIC_CENTER,     13, -6, 14, 14, 3, AIC_C_YELLOW,        255),
        AIC_RECT(AIC_BOTTOM_MID,  0, -5, 30,  7, 2, AIC_C_LIT(0x3A1C0E), 255),
    AIC_OUT,
    AIC_END
};

static bool chatarra_init(aos_app_t *app)
{
    app->desc.id      = "demo.chatarra";
    app->desc.name    = "Chatarra";
    app->desc.icon    = "CH";
    app->desc.icon_vec = AOS_ICON_GAMEPAD;
    app->desc.color_a = 0x8E4630;
    app->desc.color_b = 0x2B1810;
    app->desc.order   = 146;            /* among the games                */
    /* Neither orientation flag: standing up is the one it is laid out for
     * first, lying down puts the HUD in a column, and resize() moves the
     * game between them without losing a step. No NO_SWIPE: the system's
     * back swipe comes to chatarra_back(), which climbs one screen. */
    app->desc.flags   = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;

    /* After desc.id: the runtime files the blob under the app's id. */
    aos_icon_set_ops(app, CHATARRA_ICON, sizeof CHATARRA_ICON);

    app->create  = chatarra_create;
    app->destroy = chatarra_destroy;
    app->back    = chatarra_back;
    app->resize  = chatarra_resize;
    return true;
}

AOS_APP_ENTRY(chatarra_init);
