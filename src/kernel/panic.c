#include "panic.h"

#include "../drivers/uart.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>

#define PANIC_MAGIC_A 0x50u
#define PANIC_MAGIC_B 0xa9u

typedef struct
{
    uint8_t magic_a;
    uint8_t magic_b;
    uint8_t code;
    uint8_t detail;
} panic_record_t;

/* .noinit is not cleared by the C startup code after a watchdog reset. */
static panic_record_t g_panic_record
    __attribute__((section(".noinit")));
static uint8_t g_reset_cause
    __attribute__((section(".noinit")));

/* This runs before main and before .init9 calls it. It must fall through to
 * the next startup section, so the function is deliberately naked. */
static void kernel_early_reset_capture(void)
    __attribute__((naked, section(".init3"), used));

static void kernel_early_reset_capture(void)
{
    g_reset_cause = MCUSR;
    MCUSR = 0u;
    wdt_disable();
}

uint8_t kernel_reset_cause(void)
{
    return g_reset_cause;
}

uint8_t kernel_last_panic(uint8_t *code, uint8_t *detail)
{
    if (g_panic_record.magic_a != PANIC_MAGIC_A ||
        g_panic_record.magic_b != PANIC_MAGIC_B)
    {
        return 0u;
    }

    *code = g_panic_record.code;
    *detail = g_panic_record.detail;
    return 1u;
}

/* avr-libc normally maps every ISR without a handler to __bad_interrupt,
 * which jumps to address zero.  That looks like a reboot but does not set a
 * bit in MCUSR.  Turn it into a visible kernel fault instead. */
ISR(BADISR_vect)
{
    kernel_panic(PANIC_UNHANDLED_INTERRUPT, 0u);
}

#define PANIC_VECTOR(number)                                                \
    ISR(__vector_##number, ISR_NAKED)                                      \
    {                                                                       \
        __asm__ __volatile__("cli\n\tclr r1\n\tldi r24, 3\n\t"              \
                             "ldi r22, " #number "\n\t"                     \
                             "jmp kernel_panic\n\t");                       \
    }

PANIC_VECTOR(1)
PANIC_VECTOR(2)
PANIC_VECTOR(3)
PANIC_VECTOR(4)
PANIC_VECTOR(5)
PANIC_VECTOR(6)
PANIC_VECTOR(8)
PANIC_VECTOR(9)
PANIC_VECTOR(10)
PANIC_VECTOR(11)
PANIC_VECTOR(12)
PANIC_VECTOR(13)
PANIC_VECTOR(14)
PANIC_VECTOR(15)
PANIC_VECTOR(16)
PANIC_VECTOR(17)
PANIC_VECTOR(20)
PANIC_VECTOR(21)
PANIC_VECTOR(22)
PANIC_VECTOR(23)
PANIC_VECTOR(24)
PANIC_VECTOR(25)

#undef PANIC_VECTOR

void kernel_panic(uint8_t code, uint8_t detail)
{
    cli();

    g_panic_record.code = code;
    g_panic_record.detail = detail;
    g_panic_record.magic_a = PANIC_MAGIC_A;
    g_panic_record.magic_b = PANIC_MAGIC_B;

    uart_panic_write(code, detail);

    /* This board's bootloader can remain trapped when a watchdog reset leaves
     * WDE enabled. Stop active peripherals and restart through the reset
     * vector instead. AVR startup rebuilds SP and clears .bss; .noinit keeps
     * the panic record. */
    wdt_disable();
    UCSR0B = 0u;
    TIMSK2 = 0u;
    TCCR2B = 0u;
    __asm__ __volatile__("jmp 0" ::: "memory");

    __builtin_unreachable();
}
