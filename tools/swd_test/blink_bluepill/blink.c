/*
 * blink.c - two test programs for a Blue Pill (STM32F103C8 or one of its
 * clones), to try the Programador by SWD and by ST's serial bootloader.
 *
 *   rapido   PC13 - the board's green LED, lit when the pin is low - changes
 *            every 100 ms, so the eye tells it from the half-second blink
 *            most Blue Pills come with. Nothing else.
 *   charla   (-DCHARLA) the LED every 200 ms, and USART1 (TX on PA9, RX on
 *            PA10) at 115200 8N1: a greeting at start, a line at every
 *            change ("vuelta N: LED prendido"), and each line that comes in
 *            answered as "recibido: ...".
 *
 * It runs on the 8 MHz HSI the chip starts on: no crystal, no PLL, so it
 * behaves the same on an STM32 and on the CKS32, GD32 and APM32 clones the
 * boards often carry. Registers by address, nothing in .data or .bss (link.py
 * has no startup code to fill them), no library calls; the Cortex-M3 has its
 * own divide instruction. Time is SysTick's COUNTFLAG, polled once a
 * millisecond, and in between the UART is polled as fast as the loop goes: at
 * 115200 a byte takes 87 us, so the one-byte receive register never overruns.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC_APB2ENR   REG(0x40021018u)
#define GPIOA_CRH     REG(0x40010804u)
#define GPIOA_ODR     REG(0x4001080Cu)
#define GPIOC_CRH     REG(0x40011004u)
#define GPIOC_BSRR    REG(0x40011010u)
#define USART1_SR     REG(0x40013800u)
#define USART1_DR     REG(0x40013804u)
#define USART1_BRR    REG(0x40013808u)
#define USART1_CR1    REG(0x4001380Cu)
#define SYST_CSR      REG(0xE000E010u)
#define SYST_RVR      REG(0xE000E014u)
#define SYST_CVR      REG(0xE000E018u)

#define HSI_HZ 8000000u

#ifdef CHARLA
#define PERIOD_MS 200u
#else
#define PERIOD_MS 100u
#endif

static void led(int on)
{
    GPIOC_BSRR = on ? 1u << (13 + 16) : 1u << 13;    /* lit when low */
}

#ifdef CHARLA
static void put_c(char c)
{
    while (!(USART1_SR & (1u << 7)))    /* TXE */
        ;
    USART1_DR = (uint8_t)c;
}

static void put_s(const char *s)
{
    while (*s)
        put_c(*s++);
}

static void put_u(uint32_t n)
{
    char d[10];
    int k = 0;
    do {
        d[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (k)
        put_c(d[--k]);
}
#endif

void fault(void)
{
    /* a fault: the LED lit and still */
    led(1);
    for (;;)
        ;
}

void reset_handler(void)
{
    RCC_APB2ENR |= 1u << 4;                                 /* IOPC */
    GPIOC_CRH = (GPIOC_CRH & ~(0xFu << 20)) | (0x2u << 20); /* PC13: push-pull output, 2 MHz */
    led(0);

#ifdef CHARLA
    RCC_APB2ENR |= (1u << 2) | (1u << 14);                  /* IOPA, USART1 */
    /* PA9 alternate push-pull 50 MHz (0xB); PA10 input with its pull-up (0x8, ODR bit set)
     * so a loose wire does not read as a stream of zero bytes */
    GPIOA_CRH = (GPIOA_CRH & ~(0xFFu << 4)) | (0xBu << 4) | (0x8u << 8);
    GPIOA_ODR |= 1u << 10;
    USART1_BRR = (HSI_HZ + 115200 / 2) / 115200;            /* 69: 115942 baud, +0.6 % */
    USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);        /* UE, TE, RE */
    put_s("\r\nHola! Blue Pill de prueba de P4OS (charla)\r\n");
    put_s("Lo que escribas vuelve como \"recibido: ...\"\r\n");
    char line[64];
    uint32_t len = 0;
#endif

    SYST_RVR = HSI_HZ / 1000 - 1;           /* one millisecond on the core clock */
    SYST_CVR = 0;
    SYST_CSR = (1u << 2) | 1u;              /* CLKSOURCE = core, ENABLE */

    uint32_t ms = 0;
#ifdef CHARLA
    uint32_t turn = 0;
#endif
    int on = 0;
    for (;;) {
#ifdef CHARLA
        uint32_t sr = USART1_SR;
        if (sr & ((1u << 5) | (1u << 3))) {         /* RXNE, or ORE: reading DR clears both */
            char c = (char)USART1_DR;
            if (c == '\r' || c == '\n') {
                if (len) {
                    put_s("recibido: ");
                    for (uint32_t i = 0; i < len; i++)
                        put_c(line[i]);
                    put_s("\r\n");
                    len = 0;
                }
            } else if (len < sizeof line) {
                line[len++] = c;
            }
        }
#endif
        if (!(SYST_CSR & (1u << 16)))           /* COUNTFLAG: a millisecond went by */
            continue;
        if (++ms < PERIOD_MS)
            continue;
        ms = 0;
        on = !on;
        led(on);
#ifdef CHARLA
        turn++;
        put_s("vuelta ");
        put_u(turn);
        put_s(on ? ": LED prendido\r\n" : ": LED apagado\r\n");
#endif
    }
}
