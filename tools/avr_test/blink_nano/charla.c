/* The Nano's "L" LED (PB5) every 200 ms, and its serial port at 115200
 * (U2X, UBRR 16: 2.1 % off, as Arduino's own Serial at that speed): a
 * line at each change, and what comes in answered a line at a time. */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

/* what comes in, by interrupt: at 115200 a byte every 87 us */
static volatile char rxq[64];
static volatile unsigned char rx_w, rx_r;
ISR(USART_RX_vect)
{
    char c = (char)UDR0;
    unsigned char n = (unsigned char)((rx_w + 1) & 63);
    if (n != rx_r) { rxq[rx_w] = c; rx_w = n; }
}

static void tx(char c)
{
    while (!(UCSR0A & _BV(UDRE0))) {}
    UDR0 = c;
}

static void put(const char *s) { while (*s) tx(*s++); }

static void put_num(unsigned long n)
{
    char b[11];
    int k = 0;
    do { b[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) tx(b[--k]);
}

int main(void)
{
    DDRB |= _BV(PB5);
    UCSR0A = _BV(U2X0);
    UBRR0 = 16;
    UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    sei();
    put("\r\nHola desde P4OS: el Nano grabado por el Programador\r\n");
    char line[64];
    unsigned char k = 0;
    for (unsigned long n = 1;; n++) {
        PINB = _BV(PB5);
        put("vuelta ");
        put_num(n);
        put(PINB & _BV(PB5) ? ": LED prendido\r\n" : ": LED apagado\r\n");
        for (int t = 0; t < 200; t++) {
            _delay_ms(1);
            while (rx_r != rx_w) {
            char c = rxq[rx_r];
            rx_r = (unsigned char)((rx_r + 1) & 63);
            if (c == '\r' || c == '\n') {
                if (!k) continue;
                line[k] = 0;
                put("recibido: ");
                put(line);
                put("\r\n");
                k = 0;
            } else if (k < sizeof line - 1) {
                line[k++] = c;
            }
            }
        }
    }
}
