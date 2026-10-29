/*
 * Blink for an RP2040 (a Pico), written to test P4OS's Programador by SWD:
 * GPIO25 (the Pico's LED; a Pico W's LED is on its radio chip and stays
 * off) changes every half second, and every change is told through SEGGER
 * RTT, which the Programador's RTT console reads. A line sent down the
 * console comes back as "recibido: ...".
 *
 * No SDK and no C library: registers by address, the RTT control block
 * built by hand at a fixed place in RAM, no division (the M0+ has none),
 * nothing in .data or .bss. It runs on the ring oscillator the boot ROM
 * leaves running (~6 MHz), so the delays are loops. Built by build.sh.
 *
 * With -DDOUBLE (doble.uf2) the LED gives two short flashes and stays off
 * for a second: another firmware that the eye tells from the first, to see
 * that a write really changed what the chip runs.
 */
#include <stdint.h>

#define REG(a)          (*(volatile uint32_t *)(a))
#define RESETS_CLR      0x4000F000u     /* RESETS (0x4000C000), the atomic clear alias */
#define RESET_DONE      0x4000C008u
#define GPIO25_CTRL     0x400140CCu
#define SIO_OE_SET      0xD0000024u
#define SIO_OUT_XOR     0xD000001Cu
#define LED             (1u << 25)

/* RTT, as SEGGER_RTT.c lays it out */
#define RTT_CB          0x20001000u
#define RTT_NAME        0x20001080u
#define RTT_UP          0x20001100u
#define RTT_DN          0x20001300u
#define UP_SIZE         512u
#define DN_SIZE         64u

typedef struct { uint32_t name, buf, size, wr, rd, flags; } ring_t;
typedef struct { char id[16]; int32_t nup, ndn; ring_t up, dn; } rtt_t;
#define CB              ((volatile rtt_t *)RTT_CB)

static void echo(void);

static void rtt_init(void)
{
    volatile char *name = (volatile char *)RTT_NAME;
    const char *t = "Terminal";
    for (int i = 0; i < 9; i++) name[i] = t[i];
    CB->nup = 1;
    CB->ndn = 1;
    CB->up.name = RTT_NAME; CB->up.buf = RTT_UP; CB->up.size = UP_SIZE; CB->up.wr = 0; CB->up.rd = 0; CB->up.flags = 0;
    CB->dn.name = RTT_NAME; CB->dn.buf = RTT_DN; CB->dn.size = DN_SIZE; CB->dn.wr = 0; CB->dn.rd = 0; CB->dn.flags = 0;
    /* the ID last, so a debugger never finds half a block */
    const char *id = "SEGGER RTT";
    for (int i = 0; i < 16; i++) CB->id[i] = i < 10 ? id[i] : 0;
}

/* Into the up buffer; what does not fit is dropped (NO_BLOCK_SKIP) */
static void put(const char *s)
{
    uint32_t wr = CB->up.wr;
    for (; *s; s++) {
        uint32_t next = (wr + 1) & (UP_SIZE - 1);
        if (next == CB->up.rd) break;
        ((volatile char *)RTT_UP)[wr] = *s;
        wr = next;
    }
    CB->up.wr = wr;
}

static void put_num(uint32_t n)
{
    static const uint32_t TENS[] = { 1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u };
    char out[12];
    int k = 0;
    for (int i = 0; i < 10; i++) {
        char d = '0';
        while (n >= TENS[i]) { n -= TENS[i]; d++; }
        if (d != '0' || k || i == 9) out[k++] = d;
    }
    out[k] = 0;
    put(out);
}

/* Tenths of a second, with what comes down the console answered meanwhile */
static void wait_tenths(int n)
{
    for (int k = 0; k < n; k++) {
        for (volatile uint32_t i = 0; i < 30000; i++) {}
        echo();
    }
}

/* What came down: echoed, a line at a time */
static void echo(void)
{
    uint32_t rd = CB->dn.rd, wr = CB->dn.wr;
    if (rd == wr) return;
    char line[DN_SIZE + 1];
    int k = 0;
    while (rd != wr && k < (int)DN_SIZE) {
        char c = ((volatile char *)RTT_DN)[rd];
        rd = (rd + 1) & (DN_SIZE - 1);
        if (c != '\r' && c != '\n') line[k++] = c;
    }
    CB->dn.rd = rd;
    line[k] = 0;
    put("recibido: ");
    put(line);
    put("\n");
}

void fault(void)
{
    for (;;) {}
}

void reset_handler(void)
{
    REG(RESETS_CLR) = (1u << 5) | (1u << 8);        /* IO_BANK0, PADS_BANK0 out of reset */
    while ((REG(RESET_DONE) & ((1u << 5) | (1u << 8))) != ((1u << 5) | (1u << 8))) {}
    REG(GPIO25_CTRL) = 5;                           /* SIO */
    REG(SIO_OE_SET) = LED;
    rtt_init();
#ifdef DOUBLE
    put("Doble destello de P4OS por SWD: GPIO25, dos destellos y una pausa\n");
    for (uint32_t n = 1;; n++) {
        for (int f = 0; f < 2; f++) {
            REG(SIO_OUT_XOR) = LED;         /* on */
            wait_tenths(1);
            REG(SIO_OUT_XOR) = LED;         /* off */
            wait_tenths(2);
        }
        put("doble ");
        put_num(n);
        put("\n");
        wait_tenths(10);
    }
#else
    put("Blink de P4OS por SWD: GPIO25 cada medio segundo\n");
    for (uint32_t n = 1;; n++) {
        REG(SIO_OUT_XOR) = LED;
        put("vuelta ");
        put_num(n);
        put(n & 1 ? ": LED prendido\n" : ": LED apagado\n");
        wait_tenths(10);
    }
#endif
}
