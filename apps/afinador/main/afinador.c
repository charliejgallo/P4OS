/*
 * P4OS - Afinador (from AmoledOS #48 tuner and #12 noise meter)
 *
 * Two screens over the same microphone capture: a chromatic tuner with a real
 * needle over a cents scale, and a sound level meter with A weighting, a bar
 * meter, min / Leq / max and a minute of history. The maths lives in
 * af_dsp.c, without a line of LVGL, and is tested without the board with
 * tools/af_harness.c.
 *
 * What changed from the watch: the layout is built from the root the app is
 * given (portrait 720x1204, landscape 1280x660, two panes there), the needle
 * is eased between analyses instead of jumping, the tuner has the six guitar
 * strings as references, and the meter grew a bar with peak hold, Leq and a
 * 60 s chart. On rotation resize() rebuilds only the screen: the capture, the
 * mode and the meter's statistics stay where they were.
 *
 * Four things worth writing down before touching this:
 *
 *   - THE TWO SCREENS OPEN THE MICROPHONE AT DIFFERENT RATES, and it is not a
 *     whim. The A curve's top pole is at 12194 Hz: at a 16 kHz sample rate that
 *     lands above Nyquist and the curve folds downwards (measured: -5.74 dB of
 *     error at 6.3 kHz, against -0.58 at 32 kHz). The meter asks for 32 kHz;
 *     the tuner stays at 16, where a window of 2048 samples is 128 ms and is
 *     enough for five periods of a low E.
 *   - THE RATE THE APP ASKS FOR IS NOT ALWAYS THE ONE IT GETS. If the recorder
 *     already has the capture open, its rate rules. Everything is computed with
 *     status.sample_rate, never with the requested one, because pitch detection
 *     with the wrong rate gives a clean, convincing and wrong result.
 *   - THE REFERENCE TONE. On the watch the speaker and the microphone were the
 *     same codec, so giving the A meant closing the capture, playing, and
 *     reopening: a four-step state machine. Where the board says
 *     AOS_CAP_DUPLEX (the P4: a real speaker and two microphones) the tone just
 *     plays and the capture keeps running. The state machine stays for a
 *     board without it.
 *   - THE PEAK IS WATCHED. If the PGA saturates, the dB figure lies and the
 *     detection starts to go wrong; that is why there is a saturation warning
 *     and not just a level.
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include "af_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */

#define TICK_MS         40          /* the needle is eased at this rate      */
#define ANALISIS_MS     100         /* how often the NSDF is run             */
/* How often the LABELS are rewritten, which is not how often it is computed:
 * a label that changes adds its whole area to the frame. The needle moves on
 * every tick; the digits four times a second is what can be read anyway. */
#define PINTAR_MS       250

#define RATE_AFINADOR   16000
#define RATE_RUIDO      32000
#define VENTANA         2048        /* samples of the pitch analysis */
#define SCRATCH_FLOATS  AF_SCRATCH_FLOATS(VENTANA, RATE_RUIDO)
#define LOTE            1024        /* how much is drained at once */

#define ABRIR_TIMEOUT   2000        /* ms for the codec to open */
#define CERRAR_TIMEOUT  600         /* ms waiting for the previous capture to die */

#define CENTS_RANGO     50.0f       /* full scale of the needle */
#define ARCO_GRADOS     120         /* the scale spans +-60 degrees */
#define AFINADO_CENTS   3.0f
#define CASI_CENTS      15.0f

#define HIST_PUNTOS     240         /* one per repaint: 60 s of history */
#define DB_MIN          30
#define DB_MAX          120
#define PICO_HOLD_MS    2000

/* dB(A) = dBFS + cal.
 *
 * The starting value comes from the microphone's datasheet, not from a
 * measurement: a typical MEMS gives -26 dBFS at 94 dB SPL, and with the PGA at
 * 30 dB that puts the scale's zero near 90. It is a plausible starting point
 * and NOTHING more: until it is compared against a sound level meter, what is
 * read is relative dB. That is why the number can be corrected from the screen
 * and is stored. */
#define CAL_DEF         90
#define CAL_MIN         40
#define CAL_MAX         140

#define A4_DEF          440
#define A4_MIN          430
#define A4_MAX          450

#define TONO_SOLTAR_MS  250         /* wait for the capture to release the codec */
#define TONO_LARGO_MS   900         /* without duplex: the capture is off meanwhile */
#define TONO_DUPLEX_MS  1800        /* with duplex nothing is lost by holding it */

typedef enum { MODO_AFINAR = 0, MODO_RUIDO = 1 } modo_t;

typedef enum {
    MIC_CERRADO = 0,
    MIC_CERRANDO,       /* waiting for the previous capture to really die */
    MIC_ABRIENDO,
    MIC_ANDANDO,
    MIC_FALLO,
    TONO_SOLTANDO,      /* the capture was closed, waiting for the codec */
    TONO_SONANDO,
} mic_estado_t;

/* Standard guitar tuning, low to high. */
static const int CUERDAS[6] = { 40, 45, 50, 55, 59, 64 };

/* -------------------------------------------------------------------------- */
/* State: survives resize()                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_timer_t *timer;
    modo_t      modo;

    mic_estado_t estado;
    uint32_t     rate_pedida;
    uint32_t     rate_real;
    uint32_t     estado_desde;
    bool         reintento_16k;
    bool         tono_duplex;       /* this tone plays over a live capture */
    uint32_t     tono_hasta;

    int16_t *ventana;
    int      ventana_uso;
    float   *scratch;               /* NSDF curve + window in float */
    int16_t *lote;

    af_aweight_t aweight;
    bool         aweight_lista;

    float    hz;
    float    clarity;
    int      midi_tono;
    uint32_t ultimo_analisis;
    uint32_t hz_desde;              /* when the last clear note was heard */

    float    db;
    float    db_max, db_min;
    double   leq_suma;
    uint32_t leq_n;
    uint32_t leq_desde;
    float    pico;                  /* peak hold of the bar */
    uint32_t pico_ms;
    bool     saturado;

    uint8_t  hist[HIST_PUNTOS];     /* dB of each repaint, for the chart */
    int      hist_n;

    int      a4;
    int      cal;

    uint32_t analisis_ms_peor;
    uint32_t ultimo_log;
    uint32_t ultimo_pintado;

    float    aguja_actual;          /* in cents, eased */
    float    aguja_meta;
} af_t;

static af_t s_af;

/* -------------------------------------------------------------------------- */
/* Screen: rebuilt by resize()                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *root;
    int32_t   W, H;
    bool      land;

    lv_obj_t *tab[2];
    lv_obj_t *tab_lbl[2];
    lv_obj_t *pag[2];

    /* tuner */
    lv_obj_t *aguja;
    lv_obj_t *cubo;
    lv_obj_t *lbl_nota;
    lv_obj_t *lbl_octava;
    lv_obj_t *lbl_cents;
    lv_obj_t *lbl_hz;
    lv_obj_t *lbl_estado;
    lv_obj_t *lbl_a4;
    lv_obj_t *lbl_tono;
    lv_obj_t *cuerda[6];
    int       cuerda_on;

    /* meter */
    lv_obj_t *lbl_db;
    lv_obj_t *barra;
    lv_obj_t *marca_pico;
    int32_t   barra_x, barra_w;
    lv_obj_t *lbl_min, *lbl_leq, *lbl_max, *lbl_tiempo;
    lv_obj_t *chart;
    lv_chart_series_t *serie;
    lv_obj_t *lbl_aviso;
    lv_obj_t *lbl_info;
    lv_obj_t *lbl_cal;

    /* The last thing WRITTEN into each object. LVGL does not compare: writing
     * the same text to a label, or the same colour to a style, invalidates it
     * all the same. */
    char     txt_nota[8], txt_octava[8], txt_cents[24], txt_hz[64], txt_estado[64];
    char     txt_db[12], txt_min[12], txt_leq[12], txt_max[12], txt_tiempo[24];
    char     txt_cal[16], txt_aviso[64], txt_info[96];
    uint32_t color_nota, color_aguja, color_estado, color_barra, color_aviso;
    int32_t  angulo;
    int32_t  x_pico;
} af_ui_t;

static af_ui_t U;

/* Writes only if it changed. It is the difference between invalidating one
 * area per frame and invalidating none. */
static void set_txt(lv_obj_t *obj, char *cache, size_t cap, const char *txt)
{
    if (!obj || !txt || strncmp(cache, txt, cap - 1) == 0) {
        return;
    }
    snprintf(cache, cap, "%s", txt);
    lv_label_set_text(obj, cache);
}

static void set_color_texto(lv_obj_t *obj, uint32_t *cache, lv_color_t color)
{
    uint32_t v = lv_color_to_u32(color);
    if (!obj || *cache == v) {
        return;
    }
    *cache = v;
    lv_obj_set_style_text_color(obj, color, 0);
}

static void set_color_fondo(lv_obj_t *obj, uint32_t *cache, lv_color_t color, lv_part_t part)
{
    uint32_t v = lv_color_to_u32(color);
    if (!obj || *cache == v) {
        return;
    }
    *cache = v;
    lv_obj_set_style_bg_color(obj, color, part);
}

static uint32_t rate_del_modo(void)
{
    return s_af.modo == MODO_RUIDO ? RATE_RUIDO : RATE_AFINADOR;
}

static uint32_t ahora_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

/* -------------------------------------------------------------------------- */
/* Capture                                                                     */
/* -------------------------------------------------------------------------- */

/* Asking for a different rate means closing and reopening, BUT NOT BACK TO
 * BACK.
 *
 * Measured on the watch: aos_hal_mic_close() only removes the user, and the
 * capture task takes up to one block (~50 ms) to notice and die. If
 * mic_open() is called on the very next line, the task is still alive and
 * whoever opens hangs off the rate that was already in force. That is why
 * there is a waiting state.
 *
 * If after the deadline the capture is still open, somebody else has it (the
 * recorder): it is opened anyway and whatever rate there is gets used, which
 * is what the status reports it for and what the screen shows. */
static void mic_pedir(uint32_t rate)
{
    aos_hal_mic_close();
    s_af.rate_pedida   = rate;
    s_af.rate_real     = rate;
    s_af.estado        = MIC_CERRANDO;
    s_af.estado_desde  = ahora_ms();
    s_af.ventana_uso   = 0;
    s_af.aweight_lista = false;
    s_af.hz            = 0.0f;
}

static void mic_abrir_ya(void)
{
    s_af.estado       = MIC_ABRIENDO;
    s_af.estado_desde = ahora_ms();
    if (!aos_hal_mic_open(s_af.rate_pedida)) {
        s_af.estado = MIC_FALLO;
    }
}

static void mic_esperar_cierre(void)
{
    aos_mic_status_t st;
    bool abierta = aos_hal_mic_status(&st) && st.open;
    if (!abierta || ahora_ms() - s_af.estado_desde > CERRAR_TIMEOUT) {
        if (abierta) {
            aos_hal_log("afinador",
                        "the capture is still open (someone else has it): going with %u Hz",
                        (unsigned)st.sample_rate);
        }
        mic_abrir_ya();
    }
}

static void mic_confirmar(void)
{
    aos_mic_status_t st;
    if (!aos_hal_mic_status(&st) || !st.open) {
        if (ahora_ms() - s_af.estado_desde > ABRIR_TIMEOUT) {
            /* The codec not accepting 32 kHz is possible and is not an error:
             * it falls back to 16, which is the one the recorder uses every
             * day. */
            if (!s_af.reintento_16k && s_af.rate_pedida != RATE_AFINADOR) {
                s_af.reintento_16k = true;
                aos_hal_log("afinador", "32 kHz did not work out, going to 16 kHz");
                mic_pedir(RATE_AFINADOR);
            } else {
                s_af.estado = MIC_FALLO;
            }
        }
        return;
    }

    /* The real one, not the requested one. */
    s_af.rate_real = st.sample_rate ? st.sample_rate : RATE_AFINADOR;
    af_aweight_init(&s_af.aweight, s_af.rate_real);
    s_af.aweight_lista = true;
    s_af.estado        = MIC_ANDANDO;
    aos_hal_log("afinador", "capture at %u Hz (asked for %u), PGA %d dB",
                (unsigned)s_af.rate_real, (unsigned)s_af.rate_pedida, st.gain_db);
}

/* -------------------------------------------------------------------------- */
/* Tuner                                                                       */
/* -------------------------------------------------------------------------- */

/* Leaves the last VENTANA samples in 'ventana'. If the ring gathered more than
 * one window -because the frame was slow- the old data is discarded: what
 * matters is the sound of now, not catching up with half a second ago. */
static void drenar_ventana(void)
{
    int hay = aos_hal_mic_available();
    while (hay > VENTANA) {
        int sobra = hay - VENTANA;
        if (sobra > LOTE) {
            sobra = LOTE;
        }
        int tirado = aos_hal_mic_read(s_af.lote, sobra);
        if (tirado <= 0) {
            break;
        }
        hay -= tirado;
    }

    int n;
    while ((n = aos_hal_mic_read(s_af.lote, LOTE)) > 0) {
        if (n >= VENTANA) {
            memcpy(s_af.ventana, s_af.lote + n - VENTANA,
                   (size_t)VENTANA * sizeof(int16_t));
            s_af.ventana_uso = VENTANA;
        } else {
            int queda = s_af.ventana_uso;
            if (queda > VENTANA - n) {
                queda = VENTANA - n;
            }
            memmove(s_af.ventana,
                    s_af.ventana + (s_af.ventana_uso - queda),
                    (size_t)queda * sizeof(int16_t));
            memcpy(s_af.ventana + queda, s_af.lote,
                   (size_t)n * sizeof(int16_t));
            s_af.ventana_uso = queda + n;
        }
        if (n < LOTE) {
            break;
        }
    }
}

static void afinar_paso(void)
{
    drenar_ventana();
    if (s_af.ventana_uso < VENTANA) {
        return;
    }

    uint32_t ahora = ahora_ms();
    if (ahora - s_af.ultimo_analisis < ANALISIS_MS) {
        return;
    }
    s_af.ultimo_analisis = ahora;

    uint32_t t0 = ahora_ms();
    af_pitch_t p = af_pitch(s_af.ventana, VENTANA, s_af.rate_real,
                            s_af.scratch, SCRATCH_FLOATS);
    uint32_t costo = ahora_ms() - t0;
    if (costo > s_af.analisis_ms_peor) {
        s_af.analisis_ms_peor = costo;
    }

    s_af.clarity = p.clarity;
    if (p.hz > 0.0f) {
        /* Short smoothing: the NSDF is stable already, this only takes the
         * flicker off the last digit so the number can be read. A jump of more
         * than a semitone is a new note and is taken whole. */
        float salto = s_af.hz > 0.0f ? fabsf(12.0f * log2f(p.hz / s_af.hz)) : 99.0f;
        s_af.hz = (salto < 1.0f) ? (0.6f * s_af.hz + 0.4f * p.hz) : p.hz;
        s_af.hz_desde = ahora;
    } else if (ahora - s_af.hz_desde > 600) {
        /* A string dies away in bursts: hold the note for a moment instead of
         * blinking to "--" on every analysis that misses. */
        s_af.hz = 0.0f;
    }

    /* One line every 5 s with what the analysis costs ON THE BOARD: it is the
     * number the simulator cannot give. */
    if (ahora - s_af.ultimo_log > 5000) {
        s_af.ultimo_log = ahora;
        aos_hal_log("afinador", "NSDF worst %u ms (%u lags, %d samples, %u Hz)",
                    (unsigned)s_af.analisis_ms_peor,
                    (unsigned)(s_af.rate_real / 30 - s_af.rate_real / 1300),
                    VENTANA, (unsigned)s_af.rate_real);
        s_af.analisis_ms_peor = 0;
    }
}

static bool tono_activo(void)
{
    if (s_af.estado == TONO_SOLTANDO || s_af.estado == TONO_SONANDO) {
        return true;
    }
    return s_af.tono_duplex && (int32_t)(ahora_ms() - s_af.tono_hasta) < 0;
}

/* The needle, every tick: it travels a third of the way to where the last
 * analysis put it, so it sweeps instead of teleporting. */
static void aguja_mover(void)
{
    if (!U.aguja) {
        return;
    }
    float d = s_af.aguja_meta - s_af.aguja_actual;
    s_af.aguja_actual += (fabsf(d) < 0.2f) ? d : d * 0.35f;
    float c = s_af.aguja_actual;
    if (c >  CENTS_RANGO) c =  CENTS_RANGO;
    if (c < -CENTS_RANGO) c = -CENTS_RANGO;
    int32_t ang = (int32_t)(c / CENTS_RANGO * (float)(ARCO_GRADOS * 5));
    if (ang != U.angulo) {
        U.angulo = ang;
        aos_hand_set_angle(U.aguja, ang);
    }
}

static void aguja_meta(float cents, lv_color_t color)
{
    s_af.aguja_meta = cents;
    set_color_fondo(U.aguja, &U.color_aguja, color, 0);
}

static void cuerda_resaltar(int cual)
{
    if (cual == U.cuerda_on) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        if (!U.cuerda[i]) {
            continue;
        }
        bool on = (i == cual);
        lv_obj_set_style_bg_color(U.cuerda[i], on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    }
    U.cuerda_on = cual;
}

static void afinar_pintar(void)
{
    char buf[64];

    /* Reference tone: the screen says what is sounding. */
    if (tono_activo()) {
        af_note_t n;
        af_note_from_hz(af_note_hz(s_af.midi_tono, (float)s_af.a4),
                        (float)s_af.a4, &n);
        set_txt(U.lbl_nota, U.txt_nota, sizeof(U.txt_nota), n.name);
        set_color_texto(U.lbl_nota, &U.color_nota, AOS_C_ACCENT);
        snprintf(buf, sizeof(buf), "%d", n.octave);
        set_txt(U.lbl_octava, U.txt_octava, sizeof(U.txt_octava), buf);
        set_txt(U.lbl_cents, U.txt_cents, sizeof(U.txt_cents), _("referencia"));
        snprintf(buf, sizeof(buf), "%.2f Hz", (double)n.ref_hz);
        set_txt(U.lbl_hz, U.txt_hz, sizeof(U.txt_hz), buf);
        set_txt(U.lbl_estado, U.txt_estado, sizeof(U.txt_estado),
                s_af.estado == TONO_SOLTANDO ? _("soltando el códec") : _("sonando"));
        set_color_texto(U.lbl_estado, &U.color_estado, AOS_C_ACCENT);
        aguja_meta(0.0f, AOS_C_ACCENT);
        return;
    }

    if (s_af.estado != MIC_ANDANDO) {
        set_txt(U.lbl_nota, U.txt_nota, sizeof(U.txt_nota), "--");
        set_color_texto(U.lbl_nota, &U.color_nota, AOS_C_DIM);
        set_txt(U.lbl_octava, U.txt_octava, sizeof(U.txt_octava), "");
        set_txt(U.lbl_cents, U.txt_cents, sizeof(U.txt_cents), "");
        set_txt(U.lbl_hz, U.txt_hz, sizeof(U.txt_hz), "");
        set_txt(U.lbl_estado, U.txt_estado, sizeof(U.txt_estado),
                s_af.estado == MIC_FALLO ? _("no abrió el micrófono")
                                         : _("abriendo el micrófono"));
        set_color_texto(U.lbl_estado, &U.color_estado, AOS_C_DIM);
        aguja_meta(0.0f, AOS_C_DIM);
        cuerda_resaltar(-1);
        return;
    }

    if (s_af.hz <= 0.0f) {
        set_txt(U.lbl_nota, U.txt_nota, sizeof(U.txt_nota), "--");
        set_color_texto(U.lbl_nota, &U.color_nota, AOS_C_DIM);
        set_txt(U.lbl_octava, U.txt_octava, sizeof(U.txt_octava), "");
        set_txt(U.lbl_cents, U.txt_cents, sizeof(U.txt_cents), "");
        set_txt(U.lbl_hz, U.txt_hz, sizeof(U.txt_hz), "");
        set_txt(U.lbl_estado, U.txt_estado, sizeof(U.txt_estado),
                s_af.saturado ? _("satura: alejate del micrófono")
                              : _("tocá una cuerda"));
        set_color_texto(U.lbl_estado, &U.color_estado,
                        s_af.saturado ? AOS_C_RED : AOS_C_DIM);
        aguja_meta(0.0f, AOS_C_DIM);
        cuerda_resaltar(-1);
        return;
    }

    af_note_t n;
    af_note_from_hz(s_af.hz, (float)s_af.a4, &n);
    s_af.midi_tono = n.midi;

    lv_color_t color = AOS_C_RED;
    const char *estado;
    if (fabsf(n.cents) <= AFINADO_CENTS) {
        color  = AOS_C_GREEN;
        estado = _("afinada");
    } else if (fabsf(n.cents) <= CASI_CENTS) {
        color  = AOS_C_ORANGE;
        estado = n.cents < 0 ? _("casi: subila un poco") : _("casi: bajala un poco");
    } else {
        estado = n.cents < 0 ? _("subila") : _("bajala");
    }

    set_txt(U.lbl_nota, U.txt_nota, sizeof(U.txt_nota), n.name);
    set_color_texto(U.lbl_nota, &U.color_nota, color);
    snprintf(buf, sizeof(buf), "%d", n.octave);
    set_txt(U.lbl_octava, U.txt_octava, sizeof(U.txt_octava), buf);

    snprintf(buf, sizeof(buf), "%+.1f cents", (double)n.cents);
    set_txt(U.lbl_cents, U.txt_cents, sizeof(U.txt_cents), buf);
    snprintf(buf, sizeof(buf), "%.1f Hz   (%s%d = %.1f Hz)",
             (double)s_af.hz, n.name, n.octave, (double)n.ref_hz);
    set_txt(U.lbl_hz, U.txt_hz, sizeof(U.txt_hz), buf);
    set_txt(U.lbl_estado, U.txt_estado, sizeof(U.txt_estado),
            s_af.saturado ? _("satura") : estado);
    set_color_texto(U.lbl_estado, &U.color_estado, s_af.saturado ? AOS_C_RED : color);

    aguja_meta(n.cents, color);

    /* The string being tuned: the nearest one within a whole tone. */
    int cual = -1, mejor = 3;
    for (int i = 0; i < 6; i++) {
        int d = abs(n.midi - CUERDAS[i]);
        if (d < mejor) {
            mejor = d;
            cual = i;
        }
    }
    cuerda_resaltar(cual);
}

/* -------------------------------------------------------------------------- */
/* Noise meter                                                                 */
/* -------------------------------------------------------------------------- */

static void ruido_reiniciar(void)
{
    s_af.db_max    = 0.0f;
    s_af.db_min    = 999.0f;
    s_af.leq_suma  = 0.0;
    s_af.leq_n     = 0;
    s_af.leq_desde = ahora_ms();
    s_af.pico      = 0.0f;
}

static void ruido_paso(void)
{
    if (!s_af.aweight_lista) {
        return;
    }

    /* All in float and with the sample normalised: the hot loop has no reason
     * to go through double. */
    float suma  = 0.0f;
    int   total = 0;
    int   n;
    while ((n = aos_hal_mic_read(s_af.lote, LOTE)) > 0) {
        for (int i = 0; i < n; i++) {
            float y = af_aweight_run(&s_af.aweight,
                                     (float)s_af.lote[i] / 32768.0f);
            suma += y * y;
        }
        total += n;
        if (n < LOTE) {
            break;
        }
    }
    if (total <= 0) {
        return;
    }

    float rms  = sqrtf(suma / (float)total);
    float dbfs = (rms < 3e-9f) ? -120.0f : 20.0f * log10f(rms);
    float db   = dbfs + (float)s_af.cal;

    s_af.db = (s_af.db > 0.0f) ? (0.6f * s_af.db + 0.4f * db) : db;
    if (s_af.db > s_af.db_max) {
        s_af.db_max = s_af.db;
    }
    /* The first half second is the filter settling: it would stick a minimum
     * nobody measured. */
    uint32_t ahora = ahora_ms();
    if (ahora - s_af.leq_desde > 500) {
        if (s_af.db < s_af.db_min) {
            s_af.db_min = s_af.db;
        }
        /* Leq: the average of the ENERGY, not of the decibels. */
        s_af.leq_suma += (double)powf(10.0f, db / 10.0f);
        s_af.leq_n++;
    }
    if (s_af.db >= s_af.pico || ahora - s_af.pico_ms > PICO_HOLD_MS) {
        s_af.pico    = s_af.db;
        s_af.pico_ms = ahora;
    }
}

static lv_color_t color_db(float db)
{
    if (db >= 85.0f) return AOS_C_RED;
    if (db >= 70.0f) return AOS_C_YELLOW;
    return AOS_C_GREEN;
}

static int32_t x_de_db(float db)
{
    if (db < DB_MIN) db = DB_MIN;
    if (db > DB_MAX) db = DB_MAX;
    return (int32_t)((db - DB_MIN) / (float)(DB_MAX - DB_MIN) * (float)U.barra_w);
}

static void ruido_pintar(void)
{
    char linea[64];

    if (s_af.estado != MIC_ANDANDO) {
        set_txt(U.lbl_db, U.txt_db, sizeof(U.txt_db), "--");
        set_txt(U.lbl_aviso, U.txt_aviso, sizeof(U.txt_aviso), s_af.estado == MIC_FALLO
                ? _("no abrió el micrófono") : _("abriendo el micrófono"));
        set_color_texto(U.lbl_aviso, &U.color_aviso, AOS_C_DIM);
        return;
    }

    snprintf(linea, sizeof(linea), "%d", (int)(s_af.db + 0.5f));
    set_txt(U.lbl_db, U.txt_db, sizeof(U.txt_db), linea);
    set_color_texto(U.lbl_db, &U.color_nota, color_db(s_af.db));

    lv_bar_set_value(U.barra, (int32_t)(s_af.db + 0.5f), LV_ANIM_OFF);
    set_color_fondo(U.barra, &U.color_barra, color_db(s_af.db), LV_PART_INDICATOR);
    int32_t xp = x_de_db(s_af.pico);
    if (xp != U.x_pico) {
        U.x_pico = xp;
        lv_obj_set_x(U.marca_pico, U.barra_x + xp - 3);
    }

    /* snprintf and not lv_label_set_text_fmt: LVGL's own formatter does not
     * understand %f and prints it as the letter "f". */
    if (s_af.db_min < 900.0f) {
        snprintf(linea, sizeof(linea), "%.0f", (double)s_af.db_min);
    } else {
        snprintf(linea, sizeof(linea), "--");
    }
    set_txt(U.lbl_min, U.txt_min, sizeof(U.txt_min), linea);
    if (s_af.leq_n > 0) {
        snprintf(linea, sizeof(linea), "%.1f",
                 (double)(10.0f * log10f((float)(s_af.leq_suma / (double)s_af.leq_n))));
    } else {
        snprintf(linea, sizeof(linea), "--");
    }
    set_txt(U.lbl_leq, U.txt_leq, sizeof(U.txt_leq), linea);
    snprintf(linea, sizeof(linea), "%.0f", (double)s_af.db_max);
    set_txt(U.lbl_max, U.txt_max, sizeof(U.txt_max), linea);
    uint32_t s = (ahora_ms() - s_af.leq_desde) / 1000;
    snprintf(linea, sizeof(linea), _("Leq de %u:%02u"), (unsigned)(s / 60), (unsigned)(s % 60));
    set_txt(U.lbl_tiempo, U.txt_tiempo, sizeof(U.txt_tiempo), linea);

    aos_mic_status_t st;
    if (aos_hal_mic_status(&st)) {
        snprintf(linea, sizeof(linea), _("%u kHz  ·  PGA %d dB  ·  ponderación A, rápida"),
                 (unsigned)(s_af.rate_real / 1000), st.gain_db);
        set_txt(U.lbl_info, U.txt_info, sizeof(U.txt_info), linea);
    }

    /* The two warnings that stop the number being read as if it came from an
     * instrument: the truncated curve and the saturation. */
    if (s_af.saturado) {
        set_txt(U.lbl_aviso, U.txt_aviso, sizeof(U.txt_aviso), _("SATURA: el número no vale"));
        set_color_texto(U.lbl_aviso, &U.color_aviso, AOS_C_RED);
    } else if (s_af.rate_real < RATE_RUIDO) {
        snprintf(linea, sizeof(linea), _("a %u Hz la curva A vale hasta 4 kHz"),
                 (unsigned)s_af.rate_real);
        set_txt(U.lbl_aviso, U.txt_aviso, sizeof(U.txt_aviso), linea);
        set_color_texto(U.lbl_aviso, &U.color_aviso, AOS_C_ORANGE);
    } else {
        set_txt(U.lbl_aviso, U.txt_aviso, sizeof(U.txt_aviso),
                _("dB relativos hasta que lo calibres"));
        set_color_texto(U.lbl_aviso, &U.color_aviso, AOS_C_DIM);
    }
}

static void hist_empujar(void)
{
    int v = (int)(s_af.db + 0.5f);
    if (s_af.estado != MIC_ANDANDO) {
        return;
    }
    if (v < DB_MIN) v = DB_MIN;
    if (v > DB_MAX) v = DB_MAX;
    if (s_af.hist_n < HIST_PUNTOS) {
        s_af.hist[s_af.hist_n++] = (uint8_t)v;
    } else {
        memmove(s_af.hist, s_af.hist + 1, HIST_PUNTOS - 1);
        s_af.hist[HIST_PUNTOS - 1] = (uint8_t)v;
    }
    if (U.chart) {
        lv_chart_set_next_value(U.chart, U.serie, v);
    }
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

static void tick(lv_timer_t *t)
{
    (void)t;

    aos_mic_status_t st;
    if (aos_hal_mic_status(&st)) {
        s_af.saturado = (st.peak >= 32000);
    }

    switch (s_af.estado) {
    case MIC_CERRANDO:
        mic_esperar_cierre();
        break;

    case MIC_ABRIENDO:
        mic_confirmar();
        break;

    case MIC_ANDANDO:
        if (s_af.modo == MODO_AFINAR) {
            afinar_paso();
        } else {
            ruido_paso();
        }
        break;

    case TONO_SOLTANDO:
        if (ahora_ms() - s_af.estado_desde >= TONO_SOLTAR_MS) {
            float hz = af_note_hz(s_af.midi_tono, (float)s_af.a4);
            aos_hal_beep((int)(hz + 0.5f), TONO_LARGO_MS);
            s_af.estado       = TONO_SONANDO;
            s_af.estado_desde = ahora_ms();
        }
        break;

    case TONO_SONANDO:
        if (ahora_ms() - s_af.estado_desde >= TONO_LARGO_MS + 200) {
            s_af.reintento_16k = false;
            mic_pedir(rate_del_modo());
        }
        break;

    default:
        break;
    }

    if (s_af.modo == MODO_AFINAR) {
        aguja_mover();
    }

    uint32_t ahora = ahora_ms();
    if (ahora - s_af.ultimo_pintado < PINTAR_MS) {
        return;
    }
    s_af.ultimo_pintado = ahora;

    if (s_af.modo == MODO_AFINAR) {
        afinar_pintar();
    } else {
        ruido_pintar();
        hist_empujar();
    }
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static void pintar_tabs(void)
{
    for (int i = 0; i < 2; i++) {
        bool activa = (i == (int)s_af.modo);
        lv_obj_set_style_bg_opa(U.tab[i], activa ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(U.tab_lbl[i], activa ? AOS_C_TEXT : AOS_C_DIM, 0);
        if (activa) {
            lv_obj_remove_flag(U.pag[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(U.pag[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void poner_modo(modo_t modo)
{
    if (modo == s_af.modo) {
        return;
    }
    s_af.modo = modo;
    pintar_tabs();

    /* Each screen wants its own rate, so the capture is reopened. */
    s_af.reintento_16k  = false;
    s_af.tono_duplex    = false;
    s_af.db             = 0.0f;
    s_af.ultimo_pintado = 0;        /* repaint on the next tick */
    if (modo == MODO_RUIDO) {
        ruido_reiniciar();
    }
    mic_pedir(rate_del_modo());
}

static void ev_tab(lv_event_t *e)
{
    poner_modo((modo_t)(intptr_t)lv_event_get_user_data(e));
}

static void pintar_a4(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "A4  %d Hz", s_af.a4);
    lv_label_set_text(U.lbl_a4, buf);
}

static void pintar_cal(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "cal %d", s_af.cal);
    set_txt(U.lbl_cal, U.txt_cal, sizeof(U.txt_cal), buf);
}

static void ev_a4(lv_event_t *e)
{
    int paso = (int)(intptr_t)lv_event_get_user_data(e);
    s_af.a4 += paso;
    if (s_af.a4 < A4_MIN) s_af.a4 = A4_MIN;
    if (s_af.a4 > A4_MAX) s_af.a4 = A4_MAX;
    aos_hal_pref_set_i32("af_a4", s_af.a4);
    pintar_a4();
}

static void ev_cal(lv_event_t *e)
{
    int paso = (int)(intptr_t)lv_event_get_user_data(e);
    s_af.cal += paso;
    if (s_af.cal < CAL_MIN) s_af.cal = CAL_MIN;
    if (s_af.cal > CAL_MAX) s_af.cal = CAL_MAX;
    aos_hal_pref_set_i32("af_cal", s_af.cal);
    pintar_cal();
    ruido_reiniciar();          /* statistics across two calibrations mean nothing */
}

static void ev_reiniciar(lv_event_t *e)
{
    (void)e;
    ruido_reiniciar();
    s_af.hist_n = 0;
    if (U.chart) {
        lv_chart_set_all_values(U.chart, U.serie, LV_CHART_POINT_NONE);
    }
}

static void tono_tocar(int midi)
{
    if (s_af.estado != MIC_ANDANDO || tono_activo()) {
        return;             /* it is already sounding or has not opened yet */
    }
    s_af.midi_tono = midi > 0 ? midi : 69;      /* with no note, the A at 440 */
    s_af.ultimo_pintado = 0;

    if (aos_hal_has(AOS_CAP_DUPLEX)) {
        /* A speaker of its own: it plays over the capture, nothing to close. */
        float hz = af_note_hz(s_af.midi_tono, (float)s_af.a4);
        aos_hal_beep((int)(hz + 0.5f), TONO_DUPLEX_MS);
        s_af.tono_duplex = true;
        s_af.tono_hasta  = ahora_ms() + TONO_DUPLEX_MS;
        return;
    }
    /* The capture has to release the codec before anything sounds: they are
     * the same chip and opening the speaker tears its input channel away. */
    aos_hal_mic_close();
    s_af.estado       = TONO_SOLTANDO;
    s_af.estado_desde = ahora_ms();
}

static void ev_tono(lv_event_t *e)
{
    (void)e;
    tono_tocar(s_af.midi_tono);
}

static void ev_cuerda(lv_event_t *e)
{
    tono_tocar(CUERDAS[(int)(intptr_t)lv_event_get_user_data(e)]);
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *plano(lv_obj_t *padre, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(padre);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *tarjeta(lv_obj_t *padre, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = plano(padre, x, y, w, h);
    lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, AOS_UI_RADIUS, 0);
    return o;
}

static lv_obj_t *boton(lv_obj_t *padre, const char *texto, const lv_font_t *font,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       lv_event_cb_t cb, void *dato, lv_color_t bg)
{
    lv_obj_t *b = plano(padre, x, y, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, dato);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, texto);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* Fixed-box label: lv_obj_align_to() does not recompute when the text changes
 * length, so everything that changes goes in a fixed box. */
static lv_obj_t *caja(lv_obj_t *padre, const char *texto, const lv_font_t *font,
                      lv_color_t color, int32_t x, int32_t y, int32_t w,
                      lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(padre);
    lv_label_set_text(l, texto);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_obj_set_pos(l, x, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* An arc of the gauge's coloured band, in cents. */
static void banda(lv_obj_t *g, int32_t d, float desde, float hasta, lv_color_t color,
                  lv_opa_t opa, int32_t ancho)
{
    lv_obj_t *a = lv_arc_create(g);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, d, d);
    lv_obj_center(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, ancho, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    /* 0 degrees is three o'clock and they grow clockwise: the top is 270. */
    float g0 = 270.0f + desde / CENTS_RANGO * (ARCO_GRADOS / 2);
    float g1 = 270.0f + hasta / CENTS_RANGO * (ARCO_GRADOS / 2);
    lv_arc_set_bg_angles(a, (lv_value_precise_t)g0, (lv_value_precise_t)g1);
}

/* The needle's gauge. R is the radius; the square box is 2R wide and its
 * centre is the pivot, so only its top part holds anything. */
static void construir_cuadrante(lv_obj_t *p, int32_t cx, int32_t top, int32_t R)
{
    lv_obj_t *g = plano(p, cx - R, top, 2 * R, 2 * R);

    banda(g, 2 * R, -CENTS_RANGO, CENTS_RANGO, AOS_C_RED, LV_OPA_60, 16);
    banda(g, 2 * R, -CASI_CENTS, CASI_CENTS, AOS_C_ORANGE, LV_OPA_COVER, 16);
    banda(g, 2 * R, -AFINADO_CENTS - 1, AFINADO_CENTS + 1, AOS_C_GREEN, LV_OPA_COVER, 16);

    static const char *const NUMS[] = {
        "-50", "-40", "-30", "-20", "-10", "0", "+10", "+20", "+30", "+40", "+50", NULL
    };
    lv_obj_t *sc = lv_scale_create(g);
    lv_obj_set_size(sc, 2 * R - 44, 2 * R - 44);
    lv_obj_center(sc);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_pad_all(sc, 0, 0);
    lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(sc, -50, 50);
    lv_scale_set_total_tick_count(sc, 21);
    lv_scale_set_major_tick_every(sc, 2);
    lv_scale_set_angle_range(sc, ARCO_GRADOS);
    lv_scale_set_rotation(sc, 270 - ARCO_GRADOS / 2);
    lv_scale_set_label_show(sc, true);
    lv_scale_set_text_src(sc, (const char **)NUMS);
    lv_obj_set_style_arc_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_length(sc, 14, LV_PART_ITEMS);
    lv_obj_set_style_line_width(sc, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, AOS_C_DIM, LV_PART_ITEMS);
    lv_obj_set_style_length(sc, 28, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(sc, AOS_C_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(sc, aos_font_caption, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(sc, AOS_C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(sc, 12, LV_PART_INDICATOR);

    U.aguja = aos_hand_create(g, 8, R - 36, AOS_C_DIM);
    U.color_aguja = lv_color_to_u32(AOS_C_DIM);
    U.cubo = plano(g, R - 22, R - 22, 44, 44);
    lv_obj_set_style_radius(U.cubo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.cubo, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(U.cubo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(U.cubo, AOS_C_DIM, 0);
    lv_obj_set_style_border_width(U.cubo, 3, 0);
    U.angulo = INT32_MIN;
}

/* The note, huge. Inter's largest face with letters is 64 px, so it is scaled
 * - but in a box barely wider than two letters: what LVGL invalidates is the
 * ALREADY transformed area. */
static void construir_nota(lv_obj_t *p, int32_t cx, int32_t y, int32_t escala)
{
    const int32_t bw = 110, bh = 80;
    U.lbl_nota = lv_label_create(p);
    lv_label_set_text(U.lbl_nota, "--");
    lv_obj_set_style_text_font(U.lbl_nota, aos_font_huge, 0);
    lv_obj_set_style_text_color(U.lbl_nota, AOS_C_DIM, 0);
    lv_obj_set_style_text_align(U.lbl_nota, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(U.lbl_nota, bw, bh);
    lv_obj_set_pos(U.lbl_nota, cx - bw / 2, y + (bh * escala / 256 - bh) / 2);
    lv_obj_set_style_transform_scale(U.lbl_nota, escala, 0);
    lv_obj_set_style_transform_pivot_x(U.lbl_nota, bw / 2, 0);
    lv_obj_set_style_transform_pivot_y(U.lbl_nota, bh / 2, 0);
    lv_obj_remove_flag(U.lbl_nota, LV_OBJ_FLAG_CLICKABLE);
    U.color_nota = lv_color_to_u32(AOS_C_DIM);

    U.lbl_octava = caja(p, "", aos_font_large, AOS_C_DIM,
                        cx + bw * escala / 512 - 6, y + bh * escala / 256 - 96, 90,
                        LV_TEXT_ALIGN_LEFT);
}

static void construir_cuerdas(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const int32_t gap = 12;
    int32_t cw = (w - 5 * gap) / 6;
    for (int i = 0; i < 6; i++) {
        af_note_t n;
        af_note_from_hz(af_note_hz(CUERDAS[i], (float)s_af.a4), (float)s_af.a4, &n);
        char txt[8];
        snprintf(txt, sizeof(txt), "%s%d", n.name, n.octave);
        U.cuerda[i] = boton(p, txt, aos_font_body, x + i * (cw + gap), y, cw, h,
                            ev_cuerda, (void *)(intptr_t)i, AOS_C_CARD2);
        lv_obj_set_style_radius(U.cuerda[i], 22, 0);
    }
    U.cuerda_on = -2;
    cuerda_resaltar(-1);
}

/* [-]  A4 = 440 Hz  [+]   and the tone button. */
static void construir_controles_afinar(lv_obj_t *p, int32_t x, int32_t y, int32_t w)
{
    const int32_t h = AOS_UI_TAP_MIN;
    int32_t sw = w * (U.land ? 62 : 55) / 100;      /* the stepper */
    lv_obj_t *c = tarjeta(p, x, y, sw, h);
    lv_obj_set_style_radius(c, h / 2, 0);
    boton(c, AOS_SYM_MINUS, &aos_sym_44, 0, 0, h, h, ev_a4, (void *)(intptr_t)-1, AOS_C_CARD2);
    boton(c, AOS_SYM_PLUS, &aos_sym_44, sw - h, 0, h, h, ev_a4, (void *)(intptr_t)1, AOS_C_CARD2);
    U.lbl_a4 = caja(c, "", aos_font_body, AOS_C_TEXT, h, (h - lv_font_get_line_height(aos_font_body)) / 2,
                    sw - 2 * h, LV_TEXT_ALIGN_CENTER);
    pintar_a4();

    lv_obj_t *bt = boton(p, "", aos_font_body, x + sw + 16, y, w - sw - 16, h,
                         ev_tono, NULL, AOS_C_ACCENT);
    /* The glyph and the word are two labels: they are two fonts. */
    lv_obj_t *l = lv_obj_get_child(bt, 0);
    lv_label_set_text(l, _("Tono"));
    lv_obj_align(l, LV_ALIGN_CENTER, 24, 0);
    lv_obj_t *ic = lv_label_create(bt);
    lv_label_set_text(ic, AOS_SYM_VOLUME_HIGH);
    lv_obj_set_style_text_font(ic, &aos_sym_44, 0);
    lv_obj_set_style_text_color(ic, AOS_C_TEXT, 0);
    lv_obj_align_to(ic, l, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    U.lbl_tono = l;
}

static void construir_afinador(lv_obj_t *p, int32_t w, int32_t h)
{
    const int32_t pad = AOS_UI_PAD;
    const char *nota_pie = aos_hal_has(AOS_CAP_DUPLEX)
        ? _("tocá una cuerda para oír su nota")
        : _("escuchar y sonar no van juntos");

    if (!U.land) {
        int32_t R = (w - 2 * pad) / 2 - 8;          /* 316 */
        construir_cuadrante(p, w / 2, 8, R);
        construir_nota(p, w / 2, R + 36, 600);
        U.lbl_cents  = caja(p, "", aos_font_large, AOS_C_TEXT, pad, R + 250, w - 2 * pad,
                            LV_TEXT_ALIGN_CENTER);
        U.lbl_hz     = caja(p, "", aos_font_small, AOS_C_DIM, pad, R + 312, w - 2 * pad,
                            LV_TEXT_ALIGN_CENTER);
        U.lbl_estado = caja(p, "", aos_font_title, AOS_C_DIM, pad, R + 356, w - 2 * pad,
                            LV_TEXT_ALIGN_CENTER);
        int32_t yc = h - 3 * AOS_UI_TAP_MIN - 60;
        construir_cuerdas(p, pad, yc, w - 2 * pad, AOS_UI_TAP_MIN);
        construir_controles_afinar(p, pad, yc + AOS_UI_TAP_MIN + 20, w - 2 * pad);
        caja(p, nota_pie, aos_font_caption, AOS_C_DIM, pad, yc + 2 * AOS_UI_TAP_MIN + 40,
             w - 2 * pad, LV_TEXT_ALIGN_CENTER);
    } else {
        /* Two panes: the gauge and the reading on the left, the note and the
         * controls on the right. */
        int32_t lw = w * 52 / 100;
        int32_t R = lw / 2 - pad - 16;
        if (R > h / 2 - 8) {
            R = h / 2 - 8;
        }
        construir_cuadrante(p, pad + lw / 2, 4, R);
        U.lbl_cents  = caja(p, "", aos_font_large, AOS_C_TEXT, pad, R + 40, lw,
                            LV_TEXT_ALIGN_CENTER);
        U.lbl_hz     = caja(p, "", aos_font_small, AOS_C_DIM, pad, R + 102, lw,
                            LV_TEXT_ALIGN_CENTER);
        U.lbl_estado = caja(p, "", aos_font_title, AOS_C_DIM, pad, R + 144, lw,
                            LV_TEXT_ALIGN_CENTER);

        int32_t rx = pad + lw + 24, rw = w - rx - pad;
        construir_nota(p, rx + rw / 2, 0, 560);
        int32_t yc = h - 2 * AOS_UI_TAP_MIN - 64;
        construir_cuerdas(p, rx, yc, rw, AOS_UI_TAP_MIN);
        construir_controles_afinar(p, rx, yc + AOS_UI_TAP_MIN + 16, rw);
        caja(p, nota_pie, aos_font_caption, AOS_C_DIM, rx, h - 30, rw, LV_TEXT_ALIGN_CENTER);
    }
}

/* One statistic: a small card with the figure and what it is. */
static lv_obj_t *dato(lv_obj_t *p, const char *nombre, int32_t x, int32_t y, int32_t w,
                      int32_t h, lv_obj_t **sub)
{
    lv_obj_t *c = tarjeta(p, x, y, w, h);
    lv_obj_t *v = caja(c, "--", aos_font_large, AOS_C_TEXT, 0, 14, w, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *n = caja(c, nombre, aos_font_caption, AOS_C_DIM, 0, h - 36, w, LV_TEXT_ALIGN_CENTER);
    if (sub) {
        *sub = n;
    }
    return v;
}

static void construir_medidor(lv_obj_t *p, int32_t x, int32_t y, int32_t w)
{
    /* the figure */
    int32_t nw = w / 2 + 90;
    U.lbl_db = caja(p, "--", &aos_inter_num_144, AOS_C_TEXT, x, y, nw, LV_TEXT_ALIGN_RIGHT);
    caja(p, "dB(A)", aos_font_title, AOS_C_DIM, x + nw + 14, y + 88, 136, LV_TEXT_ALIGN_LEFT);

    /* the bar, with its peak hold and its scale */
    int32_t by = y + 176;
    lv_obj_t *c = tarjeta(p, x, by, w, 128);
    U.barra_x = 24;
    U.barra_w = w - 48;
    U.barra = lv_bar_create(c);
    lv_obj_remove_style_all(U.barra);
    lv_obj_set_size(U.barra, U.barra_w, 36);
    lv_obj_set_pos(U.barra, U.barra_x, 20);
    lv_bar_set_range(U.barra, DB_MIN, DB_MAX);
    lv_obj_set_style_bg_color(U.barra, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(U.barra, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(U.barra, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(U.barra, AOS_C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(U.barra, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(U.barra, 8, LV_PART_INDICATOR);
    lv_obj_remove_flag(U.barra, LV_OBJ_FLAG_CLICKABLE);
    U.color_barra = lv_color_to_u32(AOS_C_GREEN);

    U.marca_pico = plano(c, U.barra_x, 14, 6, 48);
    lv_obj_set_style_bg_color(U.marca_pico, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_opa(U.marca_pico, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.marca_pico, 3, 0);
    U.x_pico = -1;

    lv_obj_t *sc = lv_scale_create(c);
    lv_obj_set_size(sc, U.barra_w, 40);
    lv_obj_set_pos(sc, U.barra_x, 66);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_scale_set_mode(sc, LV_SCALE_MODE_HORIZONTAL_BOTTOM);
    lv_scale_set_range(sc, DB_MIN, DB_MAX);
    lv_scale_set_total_tick_count(sc, (DB_MAX - DB_MIN) / 5 + 1);
    lv_scale_set_major_tick_every(sc, 2);
    lv_scale_set_label_show(sc, true);
    lv_obj_set_style_line_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_length(sc, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, AOS_C_DIM, LV_PART_ITEMS);
    lv_obj_set_style_length(sc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(sc, AOS_C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(sc, aos_font_tiny, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(sc, AOS_C_DIM, LV_PART_INDICATOR);

    /* min / Leq / max */
    int32_t sy = by + 128 + 16, gap = 12, sw = (w - 2 * gap) / 3;
    U.lbl_min = dato(p, _("mínimo"), x, sy, sw, 118, NULL);
    U.lbl_leq = dato(p, "Leq", x + sw + gap, sy, sw, 118, &U.lbl_tiempo);
    U.lbl_max = dato(p, _("máximo"), x + 2 * (sw + gap), sy, sw, 118, NULL);

    U.lbl_info = caja(p, "", aos_font_caption, AOS_C_DIM, x, sy + 118 + 14, w, LV_TEXT_ALIGN_CENTER);
}

static void construir_grafico(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = tarjeta(p, x, y, w, h);
    caja(c, _("últimos 60 s"), aos_font_caption, AOS_C_DIM, 24, 14, w - 48, LV_TEXT_ALIGN_LEFT);

    int32_t cy = 48, ch = h - cy - 20, lx = 64;
    U.chart = lv_chart_create(c);
    lv_obj_remove_style_all(U.chart);
    lv_obj_set_size(U.chart, w - lx - 20, ch);
    lv_obj_set_pos(U.chart, lx, cy);
    lv_obj_set_style_line_color(U.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_line_width(U.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(U.chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(U.chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(U.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(U.chart, HIST_PUNTOS);
    lv_chart_set_axis_range(U.chart, LV_CHART_AXIS_PRIMARY_Y, DB_MIN, DB_MAX);
    lv_chart_set_div_line_count(U.chart, 4, 0);
    lv_chart_set_update_mode(U.chart, LV_CHART_UPDATE_MODE_SHIFT);
    U.serie = lv_chart_add_series(U.chart, AOS_C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_remove_flag(U.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_all_values(U.chart, U.serie, LV_CHART_POINT_NONE);
    for (int i = 0; i < s_af.hist_n; i++) {
        lv_chart_set_next_value(U.chart, U.serie, s_af.hist[i]);
    }

    /* the Y labels, beside the four division lines */
    static const char *const Y[] = { "120", "90", "60", "30" };
    int32_t lh = lv_font_get_line_height(aos_font_caption);
    for (int i = 0; i < 4; i++) {
        caja(c, Y[i], aos_font_caption, AOS_C_DIM, 8, cy + i * (ch - 1) / 3 - lh / 2, lx - 16,
             LV_TEXT_ALIGN_RIGHT);
    }
}

static void construir_controles_ruido(lv_obj_t *p, int32_t x, int32_t y, int32_t w)
{
    const int32_t h = AOS_UI_TAP_MIN;
    int32_t sw = w * 55 / 100;
    lv_obj_t *c = tarjeta(p, x, y, sw, h);
    lv_obj_set_style_radius(c, h / 2, 0);
    boton(c, AOS_SYM_MINUS, &aos_sym_44, 0, 0, h, h, ev_cal, (void *)(intptr_t)-1, AOS_C_CARD2);
    boton(c, AOS_SYM_PLUS, &aos_sym_44, sw - h, 0, h, h, ev_cal, (void *)(intptr_t)1, AOS_C_CARD2);
    U.lbl_cal = caja(c, "", aos_font_body, AOS_C_TEXT, h, (h - lv_font_get_line_height(aos_font_body)) / 2,
                     sw - 2 * h, LV_TEXT_ALIGN_CENTER);
    pintar_cal();

    lv_obj_t *b = boton(p, _("Reiniciar"), aos_font_body, x + sw + 16, y, w - sw - 16, h,
                        ev_reiniciar, NULL, AOS_C_CARD2);
    (void)b;
}

static void construir_ruido(lv_obj_t *p, int32_t w, int32_t h)
{
    const int32_t pad = AOS_UI_PAD;
    int32_t lh = lv_font_get_line_height(aos_font_small);
    if (!U.land) {
        construir_medidor(p, pad, 0, w - 2 * pad);
        int32_t gy = 176 + 128 + 16 + 118 + 14 + 28 + 16;
        int32_t cy = h - AOS_UI_TAP_MIN - 16;
        construir_grafico(p, pad, gy, w - 2 * pad, cy - gy - lh - 32);
        U.lbl_aviso = caja(p, "", aos_font_small, AOS_C_DIM, pad, cy - lh - 16, w - 2 * pad,
                           LV_TEXT_ALIGN_CENTER);
        construir_controles_ruido(p, pad, cy, w - 2 * pad);
    } else {
        int32_t lw = w * 46 / 100;
        construir_medidor(p, pad, (h - 490) / 2, lw);
        int32_t rx = pad + lw + 24, rw = w - rx - pad;
        int32_t cy = h - AOS_UI_TAP_MIN - 8;
        construir_grafico(p, rx, 0, rw, cy - lh - 28);
        U.lbl_aviso = caja(p, "", aos_font_small, AOS_C_DIM, rx, cy - lh - 12, rw,
                           LV_TEXT_ALIGN_CENTER);
        construir_controles_ruido(p, rx, cy, rw);
    }
}

static void construir(lv_obj_t *root)
{
    memset(&U, 0, sizeof(U));
    U.root = root;
    lv_obj_update_layout(root);
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;

    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* The segmented control. */
    const int32_t pad = AOS_UI_PAD;
    int32_t sw = U.land ? 560 : U.W - 2 * pad;
    int32_t sh = U.land ? 72 : 80;
    int32_t sy = U.land ? 8 : 16;
    lv_obj_t *seg = tarjeta(root, (U.W - sw) / 2, sy, sw, sh);
    lv_obj_set_style_radius(seg, sh / 2, 0);
    static const char *const NOMBRES[2] = { N_("Afinador"), N_("Sonómetro") };
    for (int i = 0; i < 2; i++) {
        U.tab[i] = boton(seg, _(NOMBRES[i]), aos_font_body, 6 + i * (sw - 12) / 2, 6,
                         (sw - 12) / 2, sh - 12, ev_tab, (void *)(intptr_t)i, AOS_C_CARD2);
        U.tab_lbl[i] = lv_obj_get_child(U.tab[i], 0);
    }

    int32_t py = sy + sh + (U.land ? 16 : 28);
    for (int i = 0; i < 2; i++) {
        U.pag[i] = plano(root, 0, py, U.W, U.H - py);
    }
    construir_afinador(U.pag[0], U.W, U.H - py);
    construir_ruido(U.pag[1], U.W, U.H - py);
    pintar_tabs();

    /* the needle starts where it is, not travelling to it */
    s_af.aguja_actual = s_af.aguja_meta;
    aguja_mover();
    s_af.ultimo_pintado = 0;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&s_af, 0, sizeof(s_af));

    int32_t v;
    s_af.a4  = aos_hal_pref_get_i32("af_a4", &v)  ? (int)v : A4_DEF;
    s_af.cal = aos_hal_pref_get_i32("af_cal", &v) ? (int)v : CAL_DEF;
    if (s_af.a4  < A4_MIN  || s_af.a4  > A4_MAX)  s_af.a4  = A4_DEF;
    if (s_af.cal < CAL_MIN || s_af.cal > CAL_MAX) s_af.cal = CAL_DEF;

    /* Everything heavy to PSRAM, of which there is plenty. */
    s_af.ventana = malloc((size_t)VENTANA * sizeof(int16_t));
    s_af.lote    = malloc((size_t)LOTE * sizeof(int16_t));
    s_af.scratch = malloc((size_t)SCRATCH_FLOATS * sizeof(float));
    if (!s_af.ventana || !s_af.lote || !s_af.scratch) {
        free(s_af.ventana);
        free(s_af.lote);
        free(s_af.scratch);
        memset(&s_af, 0, sizeof(s_af));
        aos_ui_toast(_("sin memoria"), 1500);
        return NULL;
    }

    /* Development switch: AF_MODO=ruido starts on the meter, which otherwise
     * has to be found by tapping a tab on every test. */
    const char *modo_env = getenv("AF_MODO");
    s_af.modo      = (modo_env && modo_env[0] == 'r') ? MODO_RUIDO : MODO_AFINAR;
    s_af.midi_tono = 69;
    ruido_reiniciar();

    construir(root);
    mic_pedir(rate_del_modo());

    s_af.timer = lv_timer_create(tick, TICK_MS, NULL);
    return &s_af;
}

/* The screen turned: a new screen, the same capture. */
static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    if (!inst) {
        return false;
    }
    lv_obj_clean(root);
    construir(root);
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    if (!inst) {
        return;
    }
    if (s_af.timer) {
        lv_timer_delete(s_af.timer);
        s_af.timer = NULL;
    }
    /* ALWAYS release the codec, including if you leave with the tone sounding:
     * otherwise the capture is left held against the next app that wants it. */
    aos_hal_mic_close();

    free(s_af.ventana);
    free(s_af.lote);
    free(s_af.scratch);

    if (U.root) {
        lv_obj_clean(U.root);
    }
    memset(&U, 0, sizeof(U));
    memset(&s_af, 0, sizeof(s_af));
}

static bool afinador_init(aos_app_t *app)
{
    app->desc.id       = "aos.tuner";
    app->desc.name     = "Afinador";
    app->desc.icon     = "af";
    app->desc.icon_vec = AOS_ICON_MIC;
    app->desc.color_a  = 0x40C8E0;
    app->desc.color_b  = 0x0A6070;
    app->desc.order    = 78;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE;

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(afinador_init);
