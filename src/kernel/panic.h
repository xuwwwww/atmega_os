#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

enum
{
    PANIC_STACK_OVERFLOW = 1u,
    PANIC_INVALID_SCHEDULER = 2u,
    PANIC_UNHANDLED_INTERRUPT = 3u,
    PANIC_MANUAL_TEST = 127u,
};

void kernel_panic(uint8_t code, uint8_t detail)
    __attribute__((noreturn, noinline, used, externally_visible));
uint8_t kernel_reset_cause(void);
uint8_t kernel_last_panic(uint8_t *code, uint8_t *detail);
void kernel_reboot(void) __attribute__((noreturn));

#endif
