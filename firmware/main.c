// blinking light as a test

// 8Mhz RC oscillator is default clock source
#define F_CPU 800000UL

#include <avr/io.h>
#include <util/delay.h>

int main() {
    // idk
    DDRB |= (1 << PB0);

    while(1) {
        // set that bit to true
        PORTB |= (1 << PB0);
        _delay_ms(1000);
        // set that bit to false
        PORTB &= ~(1 << PB0);
        _delay_ms(1000);
    }

    return 0;
}
