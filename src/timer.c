#include "timer.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

static volatile uint32_t g_millis = 0;

void timer_init(void)
{
    TCCR0A = (1u << WGM01);
    TCCR0B = (1u << CS01) | (1u << CS00);
    OCR0A = 249;
    TIMSK0 = (1u << OCIE0A);
}

uint32_t timer_millis(void)
{
    uint32_t millis;

    cli();
    millis = g_millis;
    sei();

    return millis;
}

ISR(TIMER0_COMPA_vect)
{
    g_millis++;
}