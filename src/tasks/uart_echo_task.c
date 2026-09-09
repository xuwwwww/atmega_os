#include "uart_echo_task.h"

#include "../drivers/uart.h"
#include "../kernel/scheduler.h"

void uart_echo_task(void)
{
    for (;;)
    {
        char c;

#ifdef UART_ECHO_TIMEOUT_TEST_MS
        if (uart_read_char_timeout(&c, UART_ECHO_TIMEOUT_TEST_MS) !=
            WAIT_ACQUIRED)
        {
            continue;
        }
#else
        uart_read_char_wait(&c);
#endif
        uart_write_char(c);
        uart_write_char(c);

        if (c == '\r')
        {
            uart_write_char('\n');
        }
    }
}
