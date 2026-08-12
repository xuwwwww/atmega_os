#include <avr/io.h>
#include <stdint.h>

int main(void)
{
    DDRB |= (1u << DDB5);

    while (1)
    {
        PORTB ^= (1u << PORTB5);

        for (volatile uint16_t i = 0; i < 500000; i++)
            ;
    }
}