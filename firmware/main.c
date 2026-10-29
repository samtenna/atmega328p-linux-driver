#define F_CPU 16000000UL // Arduino Uno R3 clock speed
#define BAUD 9600
#define SENTINEL_BYTE 0xAA
#define CMD_FRAME_LENGTH 6 // includes the crc + sentinel
#define RES_FRAME_LENGTH 5 // as above (so below)
#define CRC8_POLYNOMIAL 0x31
#define TIMEOUT_THRESHOLD 100 // times 0.01s

#include <avr/io.h>
#include <avr/sfr_defs.h>
#include <util/delay.h>
#include <util/setbaud.h>
#include <avr/interrupt.h>

typedef enum {
    STATE_IDLE, // waiting for next frame
    STATE_RECEIVING, // waiting for next byte within frame
    STATE_DONE, // received frame, waiting for command to be processed
    STATE_TIMEOUT, // tried to receive frame, timed out
} TransmissionState;

typedef enum {
    STATUS_OK = 0x00,
    STATUS_ERR_CRC = 0x01,
    STATUS_ERR_INVAL = 0x02,
    STATUS_ERR_TIMEOUT = 0x03,
} ResponseStatus;

typedef enum {
    CMD_PING = 0x00,
    CMD_ADC = 0x01,
    CMD_LED_TIMER = 0x02,
} CommandId;

static volatile int ledClockThreshold = 100;
static int ledClockCounter = 0;
volatile static uint8_t timeoutCounter = 0;

static volatile uint8_t inputBuffer[6];
static uint8_t bufferIndex = 0;
static volatile TransmissionState transmissionState = STATE_IDLE;

void start_clock() {
    // set the clock mode to CTC (clear on compare)
    TCCR0A = _BV(WGM01);
    // set the timer to go off roughly every 0.01s
    OCR0A = 156;
    // enable the interrupt
    TIMSK0 = _BV(OCIE0A);

    // set the prescaler to 1/1024, this starts the clock
    TCCR0B = _BV(CS02) | _BV(CS00);
}

void set_pin_directions() {
    // arduino led set to out
    DDRB = _BV(PB5);
}

// Spins until UDRE0 flag set (data register empty), then drop the byte into UDR0 (transmission register)
void transmit_byte(uint8_t byte) {
    while(!(UCSR0A & _BV(UDRE0)));

    UDR0 = byte;
}

uint8_t calculate_crc8(volatile uint8_t* bytes, uint8_t length) {
    uint8_t crc = 0;

    for (uint8_t i = 0; i < length; i++) {
        crc ^= bytes[i];

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

void send_response(ResponseStatus status, uint16_t payload) {
    uint8_t response[5];
    response[0] = SENTINEL_BYTE;
    response[1] = status;
    response[2] = (uint8_t)(payload & 0xFF); // low byte
    response[3] = (uint8_t)(payload >> 8); // high byte
    response[4] = calculate_crc8(response, RES_FRAME_LENGTH - 1);

    for (uint8_t i = 0; i < 5; i++) {
        transmit_byte(response[i]);
    }
}

// clock interrupt handler
ISR(TIMER0_COMPA_vect) {
    ledClockCounter++;

    if (ledClockCounter > ledClockThreshold) {
        PORTB ^= _BV(PORTB5);
        ledClockCounter = 0;
    }

    if (transmissionState == STATE_RECEIVING) {
        timeoutCounter++;

        if (timeoutCounter > TIMEOUT_THRESHOLD) {
            // timeout
            bufferIndex = 0;
            transmissionState = STATE_TIMEOUT;
            timeoutCounter = 0;
        }
    }
}

void init_uart() {
    // set baud
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;

    // enable receiver and transmitter
    UCSR0B = _BV(TXEN0) | _BV(RXEN0) | _BV(RXCIE0);

    // 8 data bits, set rx interrupt flag (no parity, 1 stop bit are defaults)
    UCSR0C |= _BV(UCSZ01) | _BV(UCSZ00);
}

ISR(USART_RX_vect) {
    uint8_t byte = UDR0;

    switch(transmissionState) {
    case STATE_IDLE: {
        // check for start of frame byte
        if (byte == SENTINEL_BYTE) {
            transmissionState = STATE_RECEIVING;
            inputBuffer[0] = 0xAA;
            bufferIndex++;
            timeoutCounter = 0;
        }

        break;
    }
    case STATE_RECEIVING: {
        inputBuffer[bufferIndex] = byte;
        bufferIndex++;

        if (bufferIndex == CMD_FRAME_LENGTH) {
            transmissionState = STATE_DONE;
            bufferIndex = 0;
        }

        break;
    }
    case STATE_TIMEOUT: break; // you can't 'receive' a timeout so don't react here
    case STATE_DONE: break; // don't care about that here
    }
}

void process_command() {
    uint8_t commandId = inputBuffer[1];

    switch(commandId) {
    case CMD_PING: {
        // 0x328P would've been better but unfortunately hex doesn't go that high
        send_response(STATUS_OK, 0x328F);

        break;
    }
    case CMD_ADC: {
        // enable if not and set prescaler to 128
        ADCSRA |= _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);

        // only care about the lower nibble
        uint8_t channelSelection = inputBuffer[2] & 0x0F;
        // write selection to register
        ADMUX = _BV(REFS0) | (channelSelection & 0x0F);

        ADCSRA |= _BV(ADSC);

        // spin on that bit until it's set back to zero
        while (ADCSRA & _BV(ADSC));
        uint16_t val = ADC;

        send_response(STATUS_OK, val);

        break;
    }
    case CMD_LED_TIMER: {
        // maybe later use arg2 as the prescaler? for now just going to use arg1 as the timer (100ths/second)
        ledClockThreshold = (uint16_t)inputBuffer[2] | ((uint16_t)inputBuffer[3] << 8);
        send_response(STATUS_OK, 0);

        break;
    }
    default: {
        send_response(STATUS_ERR_INVAL, 0);

        break;
    }
    }
}

int main() {
    set_pin_directions();

    start_clock();
    init_uart();

    sei();

    while(1) {
        while (transmissionState != STATE_DONE && transmissionState != STATE_TIMEOUT);

        if (transmissionState == STATE_TIMEOUT) {
            send_response(STATUS_ERR_TIMEOUT, 0);
            transmissionState = STATE_IDLE;

            continue;
        }

        // validate CRC
        uint8_t crc = calculate_crc8(inputBuffer, CMD_FRAME_LENGTH - 1);
        if (crc != inputBuffer[CMD_FRAME_LENGTH - 1]) {
            send_response(STATUS_ERR_CRC, 0);

            transmissionState = STATE_IDLE;
            continue;
        }

        process_command();
        transmissionState = STATE_IDLE;
    }

    return 0;
}
