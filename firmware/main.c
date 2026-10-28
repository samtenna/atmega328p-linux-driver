// Arduino Uno R3 clock speed
#define F_CPU 16000000

#include <avr/io.h>
#include <avr/sfr_defs.h>
#include <util/delay.h>
#include <avr/interrupt.h>

int extraTime = 0;

int main() {
    DDRB = _BV(PB5);

    // set the clock mode to CTC (clear on compare)
    TCCR0A = _BV(WGM01);
    // set the timer to go off roughly every 0.01s
    OCR0A = 156;
    // enable the interrupt
    TIMSK0 = _BV(OCIE0A);

    sei();

    // set the prescaler to 1/1024, this starts the clock
    TCCR0B = _BV(CS02) | _BV(CS00);

    while(1) {

    }

    return 0;
}

// handler for comparison A in timer 0
ISR(TIMER0_COMPA_vect) {
    extraTime++;

    if (extraTime > 100) {
        PORTB ^= _BV(PORTB5);
        extraTime = 0;
    }
}
