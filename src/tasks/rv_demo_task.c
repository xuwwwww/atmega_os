#include "rv_demo_task.h"

#include "../drivers/uart.h"
#include "../generated/rv_guest_image.h"
#include "../kernel/scheduler.h"
#include "../vm/rv32i.h"

#include <avr/pgmspace.h>
#include <stdint.h>

#define RV_DEMO_MEMORY_SIZE 64u
#define RV_DEMO_BUDGET_PER_SLICE 4u

static rv32_cpu_t cpu;
static uint8_t guest_memory[RV_DEMO_MEMORY_SIZE];

static const char success_message[] PROGMEM = "rv32 demo ok: x5=42";
static const char failure_message[] PROGMEM = "rv32 demo fault";

static void load_demo_program(void)
{
    for (uint8_t i = 0u; i < RV_DEMO_MEMORY_SIZE; i++)
    {
        guest_memory[i] = 0u;
    }

    for (uint8_t i = 0u; i < RV_GUEST_IMAGE_SIZE; i++)
    {
        guest_memory[i] = pgm_read_byte(&rv_guest_image[i]);
    }
}

void rv_demo_task(void)
{
    load_demo_program();
    rv32_init(&cpu, guest_memory, sizeof(guest_memory));

    while (cpu.status == RV32_RUNNING)
    {
        rv32_run(&cpu, RV_DEMO_BUDGET_PER_SLICE);
        if (cpu.status == RV32_RUNNING)
        {
            task_yield();
        }
    }

    if (cpu.status == RV32_HALTED && cpu.x[3] == 42u &&
        cpu.x[5] == 42u)
    {
        uart_write_binary_frame_P(UART_FRAME_SHELL_TEXT,
                                  (const uint8_t *)success_message,
                                  sizeof(success_message) - 1u);
    }
    else
    {
        uart_write_binary_frame_P(UART_FRAME_SHELL_TEXT,
                                  (const uint8_t *)failure_message,
                                  sizeof(failure_message) - 1u);
    }
}
