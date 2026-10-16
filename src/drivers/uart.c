#include "uart.h"

#include "../kernel/scheduler.h"
#include "../kernel/semaphore.h"
#include "../kernel/mutex.h"
#include "../kernel/timer.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>

/* ATmega328P calls its only UART vectors USART_*, while ATmega328PB names
 * the first controller USART0_*.  Keep the driver source valid for both. */
#if defined(USART_RX_vect) && !defined(USART0_RX_vect)
#define USART0_RX_vect USART_RX_vect
#define USART0_UDRE_vect USART_UDRE_vect
#endif

#ifndef UART_BAUD
#define UART_BAUD 4800UL
#endif

#ifndef UART_RX_INTERRUPT
#define UART_RX_INTERRUPT 1
#endif

#define UART_UBRR ((F_CPU / (8UL * UART_BAUD)) - 1UL)
#define UART_RX_BUFFER_SIZE 32u
#define UART_TX_BUFFER_SIZE 32u
#define UART_RX_BUFFER_MASK (UART_RX_BUFFER_SIZE - 1u)
#define UART_TX_BUFFER_MASK (UART_TX_BUFFER_SIZE - 1u)

#if (UART_RX_BUFFER_SIZE & UART_RX_BUFFER_MASK) != 0
#error "UART_RX_BUFFER_SIZE must be a power of two"
#endif

#if (UART_TX_BUFFER_SIZE & UART_TX_BUFFER_MASK) != 0
#error "UART_TX_BUFFER_SIZE must be a power of two"
#endif

static volatile uint8_t rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;
static volatile uint8_t rx_overflow = 0;
static volatile uint8_t rx_error_flags = 0;
static volatile uint8_t rx_error_count = 0;
static semaphore_t rx_available;
static volatile uint8_t tx_buffer[UART_TX_BUFFER_SIZE];
static volatile uint8_t tx_head = 0;
static volatile uint8_t tx_tail = 0;
static mutex_t tx_mutex;

#define UART_FRAME_ESCAPE 0x00u
#define UART_FRAME_SYNC 0x7eu
#define UART_FRAME_COMPRESSED 0x01u

#if LGT_SAFE_MODE
static void uart_poll_receive(void)
{
#if !UART_RX_INTERRUPT
    /* Some LGT8F-compatible bootloaders leave their watchdog path running
     * even after avr-libc's ATmega328P disable sequence.  Safe mode polls the
     * UART instead of returning to an interrupt-driven scheduler, so this is
     * also a central heartbeat for shell input and slow guest uploads. */
    wdt_reset();

    while ((UCSR0A & (1u << RXC0)) != 0u)
    {
        uint8_t status = UCSR0A;
        uint8_t received = UDR0;
        uint8_t next_head = (rx_head + 1u) & UART_RX_BUFFER_MASK;

        /* These flags describe the byte currently in UDR0, so read the
         * status first.  A bad printable hex digit must never reach the
         * loader and silently alter the SRAM image. */
        if ((status & ((1u << FE0) | (1u << DOR0) | (1u << UPE0))) != 0u)
        {
            rx_error_flags |=
                status & ((1u << FE0) | (1u << DOR0) | (1u << UPE0));
            if (rx_error_count != UINT8_MAX)
            {
                rx_error_count++;
            }
            continue;
        }

        if (next_head == rx_tail)
        {
            rx_overflow = 1u;
        }
        else
        {
            rx_buffer[rx_head] = received;
            rx_head = next_head;
        }
    }
#endif
}
#endif

static void uart_write_raw(uint8_t byte)
{
#if LGT_SAFE_MODE
    uart_poll_receive();
#endif
    /* DIAGNOSTIC: polling TX isolates the UDRE ISR from context switching. */
    while ((UCSR0A & (1u << UDRE0)) == 0u)
    {
#if LGT_SAFE_MODE
        uart_poll_receive();
        timer_poll();
#endif
    }
    UDR0 = byte;
}

static void uart_poll_write(uint8_t byte)
{
    while ((UCSR0A & (1u << UDRE0)) == 0u)
    {
    }
    UDR0 = byte;
}

static void uart_poll_write_escaped(uint8_t byte)
{
    if (byte == UART_FRAME_ESCAPE)
    {
        uart_poll_write(UART_FRAME_ESCAPE);
    }
    uart_poll_write(byte);
}

/* main prints the boot frame before the scheduler owns a task context. */
static uint8_t uart_tx_lock(void)
{
#if LGT_SAFE_MODE
    /* Safe mode never starts the native scheduler and has only one execution
     * context.  Entering scheduler-aware mutex code here would make every
     * UART byte depend on scheduler state that is not actually running. */
    return 0u;
#else
    if (scheduler_task_count() == 0u)
    {
        return 0u;
    }

    return mutex_lock(&tx_mutex);
#endif
}

static void uart_tx_unlock(uint8_t locked)
{
    if (locked)
    {
        (void)mutex_unlock(&tx_mutex);
    }
}

static void uart_write_escaped(uint8_t byte)
{
    if (byte == UART_FRAME_ESCAPE)
    {
        uart_write_raw(UART_FRAME_ESCAPE);
    }

    uart_write_raw(byte);
}

static void uart_frame_begin(uint8_t frame_type)
{
    /* 00 7e cannot occur in raw output: a literal 00 is encoded as 00 00.
     * The host can therefore abandon a truncated frame at the next marker. */
    uart_write_raw(UART_FRAME_ESCAPE);
    uart_write_raw(UART_FRAME_SYNC);
    uart_write_escaped(frame_type);
}

void uart_init(void)
{
    semaphore_init(&rx_available, 0u, UART_RX_BUFFER_SIZE - 1u);
    mutex_init(&tx_mutex);

    UBRR0H = (uint8_t)(UART_UBRR >> 8);
    UBRR0L = (uint8_t)UART_UBRR;

    UCSR0A = (1u << U2X0);
    UCSR0B = (1u << RXEN0) | (1u << TXEN0)
#if !LGT_SAFE_MODE || UART_RX_INTERRUPT
             | (1u << RXCIE0)
#endif
        ;
    UCSR0C = (1u << UCSZ01) | (1u << UCSZ00);
}

void uart_write_char(char c)
{
    uint8_t locked = uart_tx_lock();

    uart_write_escaped((uint8_t)c);
    uart_tx_unlock(locked);
}

void uart_write_compressed_P(const uint8_t *data, uint16_t byte_count,
                             uint16_t bit_count)
{
    uint8_t locked = uart_tx_lock();

    uart_frame_begin(UART_FRAME_COMPRESSED);
    uart_write_escaped((uint8_t)bit_count);
    uart_write_escaped((uint8_t)(bit_count >> 8));

    for (uint16_t i = 0; i < byte_count; i++)
    {
        uart_write_escaped(pgm_read_byte(&data[i]));
    }

    uart_tx_unlock(locked);
}

void uart_write_binary_frame(uint8_t frame_type, const uint8_t *data,
                             uint8_t byte_count)
{
    uint8_t locked = uart_tx_lock();

    uart_frame_begin(frame_type);
    uart_write_escaped(byte_count);

    for (uint8_t i = 0; i < byte_count; i++)
    {
        uart_write_escaped(data[i]);
    }

    uart_tx_unlock(locked);
}

void uart_write_binary_frame_P(uint8_t frame_type, const uint8_t *data,
                               uint8_t byte_count)
{
    uint8_t locked = uart_tx_lock();

    uart_frame_begin(frame_type);
    uart_write_escaped(byte_count);

    for (uint8_t i = 0; i < byte_count; i++)
    {
        uart_write_escaped(pgm_read_byte(&data[i]));
    }

    uart_tx_unlock(locked);
}

void uart_write_named_binary_frame(uint8_t frame_type, const char *name,
                                   uint8_t name_length,
                                   const uint8_t *data,
                                   uint8_t byte_count)
{
    uint8_t locked = uart_tx_lock();

    uart_frame_begin(frame_type);
    uart_write_escaped((uint8_t)(name_length + 2u + byte_count));
    for (uint8_t i = 0u; i < name_length; i++)
    {
        uart_write_escaped((uint8_t)name[i]);
    }
    uart_write_escaped((uint8_t)':');
    uart_write_escaped((uint8_t)' ');
    for (uint8_t i = 0u; i < byte_count; i++)
    {
        uart_write_escaped(data[i]);
    }

    uart_tx_unlock(locked);
}

void uart_write_string(const char *text)
{
    uint8_t locked = uart_tx_lock();

    while (*text != '\0')
    {
        uart_write_escaped((uint8_t)*text);
        text++;
    }

    uart_tx_unlock(locked);
}

uint8_t uart_read_char(char *out)
{
#if LGT_SAFE_MODE
    timer_poll();
    uart_poll_receive();
    if (rx_tail == rx_head)
    {
        return 0u;
    }
    *out = (char)rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1u) & UART_RX_BUFFER_MASK;
    return 1u;
#else
    if (!semaphore_try_take(&rx_available))
    {
        return 0;
    }

    *out = (char)rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1u) & UART_RX_BUFFER_MASK;
    return 1;
#endif
}

void uart_read_char_wait(char *out)
{
#if LGT_SAFE_MODE
    /* There is intentionally no second native task to schedule while the
     * shell waits. The optional RX ISR fills the ring; the legacy clone
     * profile instead polls when UART_RX_INTERRUPT=0. */
    while (!uart_read_char(out))
    {
        timer_delay_ms(1u);
    }
#else
    semaphore_take(&rx_available);

    *out = (char)rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1u) & UART_RX_BUFFER_MASK;
#endif
}

wait_result_t uart_read_char_timeout(char *out, uint32_t timeout_ms)
{
#if LGT_SAFE_MODE
    uint32_t started_at = timer_millis();

    for (;;)
    {
        if (uart_read_char(out))
        {
            return WAIT_ACQUIRED;
        }
        if ((uint32_t)(timer_millis() - started_at) >= timeout_ms)
        {
            return WAIT_TIMEOUT;
        }
        timer_delay_ms(1u);
    }
#else
    wait_result_t result =
        semaphore_take_timeout(&rx_available, timeout_ms);

    if (result == WAIT_ACQUIRED)
    {
        *out = (char)rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1u) & UART_RX_BUFFER_MASK;
    }

    return result;
#endif
}

uint8_t uart_rx_overflowed(void)
{
    uint8_t saved_sreg = SREG;
    uint8_t overflowed;

    cli();
    overflowed = rx_overflow;
    rx_overflow = 0;
    SREG = saved_sreg;

    return overflowed;
}

uint8_t uart_rx_error_flags(void)
{
    return rx_error_flags;
}

uint8_t uart_rx_error_count(void)
{
    return rx_error_count;
}

void uart_panic_write(uint8_t code, uint8_t detail)
{
    /* kernel_panic calls this with interrupts disabled. Finish any frame that
     * was already queued so the host decoder reaches a clean boundary. */
    UCSR0B &= (uint8_t)~(1u << UDRIE0);
    UCSR0A |= (1u << TXC0); /* Writing one clears the old TX-complete flag. */

    while (tx_tail != tx_head)
    {
        uart_poll_write(tx_buffer[tx_tail]);
        tx_tail = (tx_tail + 1u) & UART_TX_BUFFER_MASK;
    }

    uart_poll_write(UART_FRAME_ESCAPE);
    uart_poll_write(UART_FRAME_SYNC);
    uart_poll_write_escaped(UART_FRAME_PANIC);
    uart_poll_write_escaped(2u);
    uart_poll_write_escaped(code);
    uart_poll_write_escaped(detail);

    while ((UCSR0A & (1u << TXC0)) == 0u)
    {
    }
}

#if !LGT_SAFE_MODE || UART_RX_INTERRUPT
ISR(USART0_RX_vect)
{
    uint8_t received = UDR0;
    uint8_t next_head = (rx_head + 1u) & UART_RX_BUFFER_MASK;

    if (next_head == rx_tail)
    {
        rx_overflow = 1;
        return;
    }

    rx_buffer[rx_head] = received;
    rx_head = next_head;
#if !LGT_SAFE_MODE
    semaphore_give_from_isr(&rx_available);
#endif
}

#endif

#if !LGT_SAFE_MODE
ISR(USART0_UDRE_vect)
{
    if (tx_tail == tx_head)
    {
        UCSR0B &= ~(1u << UDRIE0);
        return;
    }

    UDR0 = tx_buffer[tx_tail];
    tx_tail = (tx_tail + 1u) & UART_TX_BUFFER_MASK;
}
#endif
