/* The Nano's "L" LED (PB5, D13) toggling every 100 ms: a sketch the eye
 * tells from the usual one-second Blink. */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
int main(void)
{
    DDRB |= _BV(PB5);
    for (;;) {
        PINB = _BV(PB5);        /* writing PIN toggles the pin */
        _delay_ms(100);
    }
}
