#include "rv_demo_task.h"

#include "../drivers/uart.h"
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

static void guest_store_instruction(uint8_t address, uint32_t instruction)
{
    guest_memory[address] = (uint8_t)instruction;
    guest_memory[address + 1u] = (uint8_t)(instruction >> 8u);
    guest_memory[address + 2u] = (uint8_t)(instruction >> 16u);
    guest_memory[address + 3u] = (uint8_t)(instruction >> 24u);
}

static void load_demo_program(void)
{
    for (uint8_t i = 0u; i < RV_DEMO_MEMORY_SIZE; i++)
    {
        guest_memory[i] = 0u;
    }

    /*
     * addi x1, x0, 20
     * addi x2, x0, 22
     * add  x3, x1, x2
     * addi x4, x0, 48
     * sw   x3, 0(x4)
     * lw   x5, 0(x4)
     * ecall                 (no handler means halt)
     */
    guest_store_instruction(0u, 0x01400093UL);
    guest_store_instruction(4u, 0x01600113UL);
    guest_store_instruction(8u, 0x002081b3UL);
    guest_store_instruction(12u, 0x03000213UL);
    guest_store_instruction(16u, 0x00322023UL);
    guest_store_instruction(20u, 0x00022283UL);
    guest_store_instruction(24u, 0x00000073UL);
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
