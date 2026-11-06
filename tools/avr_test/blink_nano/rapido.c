/* The "L" LED (D13: PB5 on a Nano or an Uno, PB7 on a Mega 2560) toggling
 * every 100 ms: a sketch the eye tells from the usual one-second Blink. */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>

#if defined(__AVR_ATmega2560__)
#define LED PB7                 /* a Mega 2560's "L" LED (D13) */
#define BOARD "el Mega 2560"
#define RX_VECT USART0_RX_vect
#else
#define LED PB5                 /* a Nano's or an Uno's "L" LED (D13) */
#define BOARD "el Nano"
#define RX_VECT USART_RX_vect
#endif
int main(void)
{
    DDRB |= _BV(LED);
    for (;;) {
        PINB = _BV(LED);        /* writing PIN toggles the pin */
        _delay_ms(100);
    }
}
