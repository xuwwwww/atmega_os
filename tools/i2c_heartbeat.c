/* Standalone wire test; deliberately independent of the OS/VM firmware. */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <util/twi.h>
#include <stdint.h>

#define NODE_ADDRESS 0x12u

static void serial_init(void)
{
    UBRR0 = (uint16_t)(F_CPU / (16UL * 4800UL) - 1UL);
    UCSR0A = 0;
    UCSR0B = (1u << TXEN0);
    UCSR0C = (1u << UCSZ01) | (1u << UCSZ00);
}

static void put_char(char c)
{
    while (!(UCSR0A & (1u << UDRE0))) {}
    UDR0 = c;
}

static void put_text_P(const char *s)
{
    char c;
    while ((c = (char)pgm_read_byte(s++)) != '\0') put_char(c);
}

#if I2C_MASTER
static void put_number(uint16_t value)
{
    char digits[5];
    uint8_t n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) put_char(digits[--n]);
}

static uint8_t last_status;

static uint8_t wait_twi(void)
{
    for (uint16_t n = 0; n < 500u; n++) {
        if (TWCR & (1u << TWINT)) {
            last_status = TW_STATUS;
            return 1;
        }
        _delay_us(100);
    }
    last_status = 0xff; /* Software timeout, not a hardware TWI status. */
    return 0;
}

static void twi_stop(void)
{
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWSTO);
    for (uint16_t n = 0; n < 500u && (TWCR & (1u << TWSTO)); n++)
        _delay_us(100);
    /* Reset controller state even if another device holds the bus low.
     * This is NOT electrical bus recovery and cannot release a stuck slave. */
    TWCR = 0;
    TWCR = (1u << TWEN);
}

static uint8_t read_heartbeat(uint8_t frame[4])
{
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWSTA);
    if (!wait_twi() || last_status != TW_START) goto failed;
    TWDR = (NODE_ADDRESS << 1u) | 1u;
    TWCR = (1u << TWINT) | (1u << TWEN);
    if (!wait_twi() || last_status != TW_MR_SLA_ACK) goto failed;
    for (uint8_t i = 0; i < 4u; i++) {
        TWCR = (1u << TWINT) | (1u << TWEN) |
               (i < 3u ? (1u << TWEA) : 0u);
        if (!wait_twi() || last_status !=
            (i < 3u ? TW_MR_DATA_ACK : TW_MR_DATA_NACK)) goto failed;
        frame[i] = TWDR;
    }
    twi_stop();
    return 1;
failed:
    twi_stop();
    return 0;
}
#else
static volatile uint16_t heartbeat;
static uint8_t response[4];
static uint8_t response_index;

ISR(TWI_vect)
{
    switch (TW_STATUS) {
    case TW_ST_SLA_ACK:
    case TW_ST_ARB_LOST_SLA_ACK:
        response[0] = 0x12;
        response[1] = (uint8_t)heartbeat;
        response[2] = (uint8_t)(heartbeat >> 8u);
        response[3] = response[0] ^ response[1] ^ response[2];
        response_index = 0;
        /* fall through */
    case TW_ST_DATA_ACK:
        TWDR = response_index < 4u ? response[response_index++] : 0xff;
        break;
    default:
        break;
    }
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA) | (1u << TWIE);
}
#endif

int main(void)
{
    cli();
    MCUSR = 0;
    wdt_disable();
    serial_init();
    /* Inputs with weak internal pull-ups; never actively drive SDA/SCL high. */
    DDRC &= ~((1u << DDC4) | (1u << DDC5));
    PORTC |= (1u << PORTC4) | (1u << PORTC5);
#if I2C_MASTER
    /* SCL = 16 MHz / (16 + 2 * 198 * 4) = 10 kHz. */
    TWSR = (1u << TWPS0);
    TWBR = 198;
    TWCR = (1u << TWEN);
    uint16_t sample = 0, previous = 0;
    uint8_t have_previous = 0;
    put_text_P(PSTR("I2C master COM5 address=18 scl=10000\r\n"));
    for (;;) {
        uint8_t frame[4];
        put_text_P(PSTR("sample=")); put_number(++sample);
        if (!read_heartbeat(frame)) {
            put_text_P(PSTR(" OFFLINE twi_status=")); put_number(last_status);
            put_text_P(PSTR(" pins=")); put_number(PINC & 0x30u);
            have_previous = 0;
        } else if (frame[0] != NODE_ADDRESS ||
                   frame[3] != (uint8_t)(frame[0] ^ frame[1] ^ frame[2])) {
            put_text_P(PSTR(" BAD_FRAME"));
            have_previous = 0;
        } else {
            uint16_t current = frame[1] | ((uint16_t)frame[2] << 8u);
            put_text_P(have_previous && current == previous ?
                       PSTR(" STALE heartbeat=") : PSTR(" ONLINE heartbeat="));
            put_number(current);
            previous = current;
            have_previous = 1;
        }
        put_text_P(PSTR("\r\n"));
        _delay_ms(500);
    }
#else
    TWAR = NODE_ADDRESS << 1u;
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA) | (1u << TWIE);
    sei();
    put_text_P(PSTR("I2C slave COM7 address=18 ready\r\n"));
    for (;;) {
        _delay_ms(1);
        uint8_t saved_sreg = SREG;
        cli();
        heartbeat++;
        SREG = saved_sreg;
    }
#endif
}
