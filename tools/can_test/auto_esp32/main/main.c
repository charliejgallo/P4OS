/* A car on a CAN bus, for learning CAN with P4OS's CAN app: an ESP32 with an
 * SN65HVD230 on IO21 (TX, to CTX) and IO22 (RX, from CRX), wired as in
 * docs/img/bench-can.svg.
 *
 * It drives a loop of about a minute (idle, pulls away through five gears,
 * cruises, brakes, stops) and sends what an engine and a dashboard put on
 * the bus, with the ids and the coding of the simulator's car
 * (sim/io_signal_sim.c), so the example senales.dbc the CAN app writes
 * decodes it as it is:
 *
 *   0x0C0  every 10 ms   RPM x4 in bytes 2-3 (Motorola), a counter in byte 7;
 *                        also throttle % in byte 0, gear in byte 4, brake in
 *                        byte 5 (not in the example .dbc: adding them is an
 *                        exercise, the README has the lines)
 *   0x1A0  every 20 ms   speed x100 in km/h, bytes 0-1 (Motorola)
 *   0x3E8  every 500 ms  coolant and oil temperature, +40 C, bytes 0 and 1
 *   0x18FEF100  100 ms   an extended id (J1939 style), a counter in bytes 1-2
 *
 * It also answers OBD-II, mode 01, as the simulator's car does, so the CAN
 * app's OBD tab reads it: two units, the engine on 0x7E8 with some twenty
 * PIDs from the car's own state (load, air flow, fuel level and use, oil,
 * the odometer...) and the gearbox on 0x7E9, asked at 0x7DF (or 18DB33F1
 * with 29-bit ids). From the Send tab: 7DF with 02 01 0C 00 00 00 00 00
 * asks for the engine speed.
 *
 * The console (UART0, the board's USB-serial converter at 115200, so the
 * P4's Terminal on usb0) shows a dashboard line a second, every frame
 * received, the state changes and the kind of each bus error (bit, form,
 * stuff, ACK), which the P4 does not show yet. One-key commands: h for the
 * list. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

#define PIN_TX 21
#define PIN_RX 22
#define TX_SLOTS 32     /* frames handed to the driver stay ours until sent */
#define TX_QUEUE 16

static twai_node_handle_t s_node;
static uint32_t s_rate = 500000;
static QueueHandle_t s_rxq, s_evq;

/* counted in the driver's callbacks (an ISR), read by the task */
static volatile uint32_t s_err_bit, s_err_form, s_err_stuff, s_err_ack, s_arb_lost;
static volatile uint32_t s_tx_ok, s_tx_fail;
static uint32_t s_rx, s_dropped, s_obd_q, s_obd_a;

typedef struct { uint32_t id; uint8_t len, ext, rtr, data[8]; } frame_t;
typedef struct { twai_error_state_t from, to; } state_ev_t;

static const char *state_name(twai_error_state_t s)
{
    static const char *const N[] = { "activo", "aviso", "pasivo", "bus off" };
    return s <= TWAI_ERROR_BUS_OFF ? N[s] : "?";
}

/* ---- the driver's callbacks (ISR) ---- */

static bool on_rx(twai_node_handle_t node, const twai_rx_done_event_data_t *ed, void *user)
{
    (void)ed; (void)user;
    uint8_t buf[8];
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof buf };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) return false;
    frame_t r = { .id = f.header.id, .len = f.header.dlc > 8 ? 8 : f.header.dlc,
                  .ext = f.header.ide, .rtr = f.header.rtr };
    memcpy(r.data, buf, r.len);
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_rxq, &r, &hp);
    return hp == pdTRUE;
}

static bool on_error(twai_node_handle_t node, const twai_error_event_data_t *ed, void *user)
{
    (void)node; (void)user;
    if (ed->err_flags.bit_err) s_err_bit++;
    if (ed->err_flags.form_err) s_err_form++;
    if (ed->err_flags.stuff_err) s_err_stuff++;
    if (ed->err_flags.ack_err) s_err_ack++;
    if (ed->err_flags.arb_lost) s_arb_lost++;
    return false;
}

static bool on_state(twai_node_handle_t node, const twai_state_change_event_data_t *ed, void *user)
{
    (void)node; (void)user;
    state_ev_t e = { ed->old_sta, ed->new_sta };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_evq, &e, &hp);
    return hp == pdTRUE;
}

static bool on_tx(twai_node_handle_t node, const twai_tx_done_event_data_t *ed, void *user)
{
    (void)node; (void)user;
    if (ed->is_tx_success) s_tx_ok++;
    else s_tx_fail++;
    return false;
}

/* ---- the node ---- */

static bool node_start(void)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = { .tx = PIN_TX, .rx = PIN_RX, .quanta_clk_out = -1, .bus_off_indicator = -1 },
        .bit_timing = { .bitrate = s_rate },
        .tx_queue_depth = TX_QUEUE,
        .fail_retry_cnt = -1,               /* like the P4: retry until it goes out or bus off */
    };
    if (twai_new_node_onchip(&cfg, &s_node) != ESP_OK) {
        printf("No se pudo abrir el CAN a %lu bit/s\r\n", (unsigned long)s_rate);
        s_node = NULL;
        return false;
    }
    twai_event_callbacks_t cb = { .on_rx_done = on_rx, .on_error = on_error,
                                  .on_state_change = on_state, .on_tx_done = on_tx };
    twai_node_register_event_callbacks(s_node, &cb, NULL);
    twai_node_enable(s_node);
    return true;
}

static void node_stop(void)
{
    if (!s_node) return;
    twai_node_disable(s_node);
    twai_node_delete(s_node);
    s_node = NULL;
}

static void send(uint32_t id, bool ext, const uint8_t *data, uint8_t len)
{
    static twai_frame_t slot[TX_SLOTS];
    static uint8_t buf[TX_SLOTS][8];
    static unsigned next;
    if (!s_node) return;
    unsigned k = next % TX_SLOTS;
    memcpy(buf[k], data, len);
    slot[k] = (twai_frame_t){ .header = { .id = id, .dlc = len, .ide = ext }, .buffer = buf[k], .buffer_len = len };
    /* no wait: with nobody acknowledging the queue fills, and what does not
     * fit is counted as dropped instead of holding the car back */
    if (twai_node_transmit(s_node, &slot[k], 0) == ESP_OK) next++;
    else s_dropped++;
}

/* ---- the car ---- */

static struct {
    float speed;        /* km/h */
    float rpm;
    int gear;           /* 0 neutral */
    float throttle;     /* 0-100 % */
    bool brake;
    float coolant, oil; /* C */
    float maf;          /* air into the engine, g/s */
    float fuel;         /* litres in a 50 l tank */
    double km;          /* the odometer */
} car = { .rpm = 800, .coolant = 20, .oil = 20, .fuel = 31, .km = 123456.7 };

#define TANK_L 50.0f
#define AMBIENT_C 22

/* litres an hour: air over a stoichiometric 14.7 and petrol's 740 g/l */
static float fuel_lph(void) { return car.maf * 3600.0f / (14.7f * 740.0f); }

static bool s_running = true;

/* km/h per 1000 rpm in each gear: a small petrol car */
static const float KMH_PER_KRPM[] = { 0, 7.5f, 13.5f, 20.0f, 27.0f, 34.0f };

/* one step of 10 ms along the loop: idle 4 s, pull away to ~100 km/h, cruise,
 * brake to a stop; about 60 s round */
static void car_step(float t)
{
    const float dt = 0.01f;
    float phase = fmodf(t, 60.0f);
    if (phase < 4) {                             /* stopped, idling */
        car.throttle = 0; car.brake = false;
        car.speed = 0;
    } else if (phase < 30) {                     /* pulling away, harder at first */
        car.brake = false;
        car.throttle = car.speed < 95 ? 70 - car.speed * 0.3f : 22;
        float a = car.throttle * 0.09f - car.speed * 0.02f;            /* km/h a second: 0-100 in ~25 s */
        car.speed += a * dt;
    } else if (phase < 45) {                     /* cruising, a little up and down */
        car.brake = false;
        car.throttle = 22 + 6 * sinf(t * 0.8f);
        car.speed += (100 + 4 * sinf(t * 0.5f) - car.speed) * dt * 0.5f;
    } else {                                     /* braking to a stop */
        car.throttle = 0; car.brake = car.speed > 0.5f;
        car.speed -= (car.speed > 30 ? 9.0f : 5.0f) * dt;              /* km/h a second */
        if (car.speed < 0) car.speed = 0;
    }
    /* the gear: up at 3000 rpm, down at 1300, neutral when stopped */
    if (car.speed < 1) car.gear = phase < 4 || car.brake ? 0 : 1;
    else {
        if (car.gear == 0) car.gear = 1;
        float r = car.speed / KMH_PER_KRPM[car.gear] * 1000;
        if (r > 3000 && car.gear < 5) car.gear++;
        else if (r < 1300 && car.gear > 1) car.gear--;
    }
    float target = car.gear ? car.speed / KMH_PER_KRPM[car.gear] * 1000 : 800 + car.throttle * 20;
    if (target < 800) target = 800;              /* the clutch slips below idle */
    car.rpm += (target - car.rpm) * 0.2f;
    /* warming up: coolant to 90 C in a few minutes, oil slower */
    car.coolant += (90 - car.coolant) * dt / 90;
    car.oil += (car.coolant - 5 - car.oil) * dt / 150;
    /* about 2.5 g/s at idle, 15-25 g/s cruising or pulling */
    car.maf = 2.5f * car.rpm / 800 * (1 + car.throttle / 25);
    car.fuel -= fuel_lph() * dt / 3600;
    if (car.fuel < 4) car.fuel = 45;             /* refilled, so a long test never runs dry */
    car.km += car.speed * dt / 3600;
}

static void car_send(uint32_t tick)
{
    uint16_t r4 = (uint16_t)(car.rpm * 4);
    uint8_t m[8] = { (uint8_t)car.throttle, 0, (uint8_t)(r4 >> 8), (uint8_t)r4,
                     (uint8_t)car.gear, car.brake, 0, (uint8_t)tick };
    send(0x0C0, false, m, 8);
    if (tick % 2 == 0) {
        uint16_t v = (uint16_t)(car.speed * 100);
        uint8_t s[4] = { (uint8_t)(v >> 8), (uint8_t)v, 0, 0 };
        send(0x1A0, false, s, 4);
    }
    if (tick % 10 == 0) {
        uint8_t j[8] = { 0xFF, (uint8_t)(tick >> 8), (uint8_t)tick, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        send(0x18FEF100, true, j, 8);
    }
    if (tick % 50 == 0) {
        uint8_t c[2] = { (uint8_t)(car.coolant + 40), (uint8_t)(car.oil + 40) };
        send(0x3E8, false, c, 2);
    }
}

/* ---- OBD-II, mode 01 ---- */

/* Two units, as the simulator's car (sim/io_signal_sim.c): the engine
 * answers on 7E8 (18DAF110 with 29-bit ids) and knows ENGINE_PIDS, the
 * gearbox on 7E9 (18DAF118) only 01 and 0D. A question to 7DF (18DB33F1)
 * reaches both, one to 7E0 or 7E1 (18DA10F1, 18DA18F1) only that one. The
 * PIDs 20, 40... in a list say that the next map is there too. */
static const uint8_t ENGINE_PIDS[] = { 0x01, 0x04, 0x05, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1C, 0x1F, 0x20, 0x2F,
                                       0x33, 0x40, 0x42, 0x46, 0x51, 0x5C, 0x5E, 0x60, 0x80, 0xA0, 0xA6 };
static const uint8_t GEARBOX_PIDS[] = { 0x01, 0x0D };

static bool knows(int ecu, uint8_t pid)
{
    const uint8_t *p = ecu ? GEARBOX_PIDS : ENGINE_PIDS;
    size_t n = ecu ? sizeof GEARBOX_PIDS : sizeof ENGINE_PIDS;
    return memchr(p, pid, n) != NULL;
}

/* the answer's bytes A, B... for a PID, or -1 if the unit does not know it */
static int obd_value(int ecu, uint8_t pid, uint8_t *o)
{
    uint32_t v;
    if ((pid & 0x1F) == 0) {                      /* a map: bit 7 of A is pid + 1 */
        if (pid && !knows(ecu, pid)) return -1;
        v = 0;
        for (int k = 1; k <= 32; k++)
            if (knows(ecu, (uint8_t)(pid + k))) v |= 1u << (32 - k);
        o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = (uint8_t)v;
        return 4;
    }
    if (!knows(ecu, pid)) return -1;
    switch (pid) {
    /* monitor status: no fault light, no codes; the engine's tests as a petrol car's */
    case 0x01: o[0] = 0; o[1] = ecu ? 0 : 0x07; o[2] = ecu ? 0 : 0x65; o[3] = 0; return 4;
    case 0x04: o[0] = (uint8_t)((15 + car.throttle * 0.8f) * 255 / 100); return 1;
    case 0x05: o[0] = (uint8_t)(car.coolant + 40); return 1;
    case 0x0C: v = (uint32_t)(car.rpm * 4); o[0] = v >> 8; o[1] = (uint8_t)v; return 2;
    case 0x0D: o[0] = (uint8_t)car.speed; return 1;
    case 0x0F: o[0] = AMBIENT_C + 5 + 40; return 1;
    case 0x10: v = (uint32_t)(car.maf * 100); o[0] = v >> 8; o[1] = (uint8_t)v; return 2;
    case 0x11: o[0] = (uint8_t)(car.throttle * 255 / 100); return 1;
    case 0x1C: o[0] = 6; return 1;                /* EOBD */
    case 0x1F: v = (uint32_t)(esp_timer_get_time() / 1000000); o[0] = v >> 8; o[1] = (uint8_t)v; return 2;
    case 0x2F: o[0] = (uint8_t)(car.fuel / TANK_L * 255); return 1;
    case 0x33: o[0] = 101; return 1;              /* kPa, sea level */
    case 0x42: v = 14100; o[0] = v >> 8; o[1] = (uint8_t)v; return 2;     /* mV, alternator charging */
    case 0x46: o[0] = AMBIENT_C + 40; return 1;
    case 0x51: o[0] = 1; return 1;                /* petrol */
    case 0x5C: o[0] = (uint8_t)(car.oil + 40); return 1;
    case 0x5E: v = (uint32_t)(fuel_lph() * 20); o[0] = v >> 8; o[1] = (uint8_t)v; return 2;
    case 0xA6: v = (uint32_t)(car.km * 10); o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = (uint8_t)v; return 4;
    default: return -1;
    }
}

/* a question, as a single ISO-TP frame (byte 0 the length, then mode 01 and
 * the PID), answered by each unit it is for that knows the PID; returns how
 * many answered */
static int obd_answer(const frame_t *q)
{
    int to;                                       /* -1 nobody, 0 engine, 1 gearbox, 2 both */
    if (!q->ext) to = q->id == 0x7DF ? 2 : q->id == 0x7E0 ? 0 : q->id == 0x7E1 ? 1 : -1;
    else to = q->id == 0x18DB33F1 ? 2 : q->id == 0x18DA10F1 ? 0 : q->id == 0x18DA18F1 ? 1 : -1;
    if (to < 0 || q->rtr || q->len < 3 || q->data[0] < 2 || q->data[0] > 7 || q->data[1] != 0x01) return 0;
    int answered = 0;
    for (int ecu = 0; ecu < 2; ecu++) {
        if (to != 2 && to != ecu) continue;
        uint8_t a[8] = { 0, 0x41, q->data[2], 0, 0, 0, 0, 0 };
        int n = obd_value(ecu, q->data[2], &a[3]);
        if (n < 0) continue;                      /* a PID it does not know: no answer, as on most cars */
        a[0] = (uint8_t)(2 + n);
        send(q->ext ? (ecu ? 0x18DAF118 : 0x18DAF110) : 0x7E8 + ecu, q->ext, a, 8);
        answered++;
    }
    return answered;
}

/* ---- the console ---- */

static void help(void)
{
    printf("\r\nTeclas: p para o arranca el auto (deja de mandar), 1/2/5/m velocidad del bus\r\n"
           "125k/250k/500k/1M, r recuperar de bus off, e borrar contadores, h esta ayuda\r\n\r\n");
}

static void set_rate(uint32_t r)
{
    node_stop();
    s_rate = r;
    if (node_start()) printf("Bus a %lu kbit/s\r\n", (unsigned long)(r / 1000));
}

static void key(int c)
{
    switch (c) {
    case 'p': s_running = !s_running; printf(s_running ? "Auto en marcha\r\n" : "Auto parado: no manda nada\r\n"); break;
    case '1': set_rate(125000); break;
    case '2': set_rate(250000); break;
    case '5': set_rate(500000); break;
    case 'm': set_rate(1000000); break;
    case 'r':
        printf(s_node && twai_node_recover(s_node) == ESP_OK ? "Recuperando: espera 128 x 11 bits recesivos\r\n"
                                                              : "Sólo se recupera desde bus off\r\n");
        break;
    case 'e':
        s_err_bit = s_err_form = s_err_stuff = s_err_ack = s_arb_lost = 0;
        s_tx_ok = s_tx_fail = s_rx = s_dropped = s_obd_q = s_obd_a = 0;
        printf("Contadores en cero\r\n");
        break;
    case 'h': case '?': help(); break;
    default: break;
    }
}

static void dashboard(void)
{
    twai_node_status_t st = { 0 };
    if (s_node) twai_node_get_info(s_node, &st, NULL);
    printf("%5.1f km/h %dª %4.0f rpm acel %3.0f%%%s agua %2.0f°C aceite %2.0f°C nafta %4.1f l %.1f km | "
           "%s TEC %u REC %u | tx %lu rx %lu desc %lu OBD %lu/%lu | "
           "errores bit %lu forma %lu relleno %lu ack %lu arbitraje %lu\r\n",
           car.speed, car.gear, car.rpm, car.throttle, car.brake ? " FRENO" : "", car.coolant, car.oil,
           car.fuel, car.km, state_name(st.state), st.tx_error_count, st.rx_error_count,
           (unsigned long)s_tx_ok, (unsigned long)s_rx, (unsigned long)s_dropped,
           (unsigned long)s_obd_q, (unsigned long)s_obd_a,
           (unsigned long)s_err_bit, (unsigned long)s_err_form, (unsigned long)s_err_stuff,
           (unsigned long)s_err_ack, (unsigned long)s_arb_lost);
}

void app_main(void)
{
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    s_rxq = xQueueCreate(64, sizeof(frame_t));
    s_evq = xQueueCreate(8, sizeof(state_ev_t));
    printf("\r\nAuto CAN de P4OS: TX IO%d a CTX, RX IO%d desde CRX\r\n", PIN_TX, PIN_RX);
    if (node_start()) printf("Bus a %lu kbit/s, modo normal\r\n", (unsigned long)(s_rate / 1000));
    help();

    TickType_t wake = xTaskGetTickCount();
    int printed = 0;
    for (uint32_t tick = 0;; tick++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
        float t = (float)(esp_timer_get_time() / 1000) / 1000.0f;
        car_step(t);
        if (s_running) car_send(tick);

        frame_t r;
        while (xQueueReceive(s_rxq, &r, 0) == pdTRUE) {
            s_rx++;
            int answered = obd_answer(&r);
            if (answered) { s_obd_q++; s_obd_a += answered; }
            if (printed < 20) {                       /* at most 20 lines a second */
                printf("RX %s%03lX [%u]", r.ext ? "x" : "", (unsigned long)r.id, r.len);
                if (r.rtr) printf(" remota");
                else for (int i = 0; i < r.len; i++) printf(" %02X", r.data[i]);
                if (answered) printf("  OBD PID %02X: %d respuesta%s", r.data[2], answered, answered > 1 ? "s" : "");
                printf("\r\n");
                if (++printed == 20) printf("(demasiadas tramas: muestro 20 por segundo)\r\n");
            }
        }
        state_ev_t e;
        while (xQueueReceive(s_evq, &e, 0) == pdTRUE)
            printf("Estado: %s -> %s\r\n", state_name(e.from), state_name(e.to));

        uint8_t c;
        while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) key(c);

        if (tick % 100 == 0) {
            dashboard();
            printed = 0;
        }
        fflush(stdout);
    }
}
