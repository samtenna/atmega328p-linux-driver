// Arduino Uno R3 clock speed
#include <stdint.h>
#define F_CPU 16000000UL
#define BAUD 9600
#define SENTINEL_BYTE 0xAA
#define CMD_FRAME_LENGTH 6 // includes the crc + sentinel
#define CRC8_POLYNOMIAL 0x31

#include <avr/io.h>
#include <avr/sfr_defs.h>
#include <util/delay.h>
#include <util/setbaud.h>
#include <avr/interrupt.h>

static int ledClockCounter = 0;

typedef enum {
    STATE_IDLE, // waiting for next frame
    STATE_RECEIVING, // waiting for next byte within frame
    STATE_DONE, // received frame, waiting for command to be processed
} TransmissionState;

typedef enum {
    STATUS_OK,
    STATUS_ERR_CRC,
    STATUS_ERR_INVAL,
    STATUS_ERR_TIMEOUT,
} ResponseStatus;

typedef enum {
    CMD_PING,
    CMD_ADC,
    CMD_LED_TIMER,
} CommandId;

static volatile uint8_t transmissionBuffer[6];
static uint8_t bufferIndex = 0;
static volatile TransmissionState transmissionState = STATE_IDLE;

void setupLedInterrupt() {
    // set the clock mode to CTC (clear on compare)
    TCCR0A = _BV(WGM01);
    // set the timer to go off roughly every 0.01s
    OCR0A = 156;
    // enable the interrupt
    TIMSK0 = _BV(OCIE0A);

    // set the prescaler to 1/1024, this starts the clock
    TCCR0B = _BV(CS02) | _BV(CS00);
}

void setPinDirections() {
    // arduino led set to out
    DDRB = _BV(PB5);
}

// LED interrupt handler
ISR(TIMER0_COMPA_vect) {
    ledClockCounter++;

    if (ledClockCounter > 100) {
        PORTB ^= _BV(PORTB5);
        ledClockCounter = 0;
    }
}

void initUART() {
    // set baud
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;

    // enable receiver and transmitter
    UCSR0B = _BV(TXEN0) | _BV(RXEN0);

    // 8 data bits, set rx interrupt flag (no parity, 1 stop bit are defaults)
    UCSR0C |= _BV(UCSZ01) | _BV(UCSZ00) | _BV(RXCIE0);
}

ISR(USART_RX_vect) {
    uint8_t byte = UDR0;

    switch(transmissionState) {
    case STATE_IDLE: {
        // check for start of frame byte
        if (byte == SENTINEL_BYTE) {
            transmissionState = STATE_RECEIVING;
            transmissionBuffer[0] = 0xAA;
            bufferIndex++;
        }

        break;
    }
    case STATE_RECEIVING: {
        transmissionBuffer[bufferIndex] = byte;
        bufferIndex++;

        if (bufferIndex == CMD_FRAME_LENGTH) {
            transmissionState = STATE_DONE;
            bufferIndex = 0;
        }

        break;
    }
    case STATE_DONE: break; // don't care about that here
    }
}

// Spins until UDRE0 flag set (data register empty), then drop the byte into UDR0 (transmission register)
void transmitByte(uint8_t byte) {
    while(!(UCSR0A & _BV(UDRE0)));

    UDR0 = byte;
}

uint8_t calculateCRC8(volatile uint8_t* bytes, uint8_t length) {
    uint8_t crc = 0;

    for (uint8_t i = 0; i < length; i++) {
        crc ^= transmissionBuffer[i];

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ CRC8_POLYNOMIAL;
            } else {
                crc = (crc << 1);
            }
        }
    }

    return crc;
}

void sendResponse() {}

int main() {
    setPinDirections();

    sei();

    setupLedInterrupt();
    initUART();

    while(1) {
        while (transmissionState != STATE_DONE);

        // validate CRC
        uint8_t crc = calculateCRC8(transmissionBuffer, CMD_FRAME_LENGTH - 1);
        if (crc != transmissionBuffer[CMD_FRAME_LENGTH - 1]) {
            // TODO: send retry response

            transmissionState = STATE_IDLE;
            continue;
        }

        // process command
    }

    return 0;
}
