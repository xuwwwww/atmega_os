#include "timer.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

int main(void)
{
    DDRB |= (1u << DDB5);


    timer_init();
    sei();

    uint32_t last = 0;

    while (1)
    {
        uint32_t now = timer_millis();

        if (PORTB & (1u << 5)) //lighted
        {
            if (now - last >= 100)
                {
                    last = now;
                    PORTB ^= (1u << PORTB5);
                }
        }else{
            if (now - last >= 1000)
                {
                    last = now;
                    PORTB ^= (1u << PORTB5);
                }
        }
        
    }
}