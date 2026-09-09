#include "rv_demo_task.h"

#include "../drivers/uart.h"
#include "../generated/rv_guest_image.h"
#include "../kernel/scheduler.h"
#include "../vm/rv32i.h"

#include <avr/pgmspace.h>
#include <stdint.h>

#define RV_DEMO_MEMORY_SIZE 192u
#define RV_DEMO_BUDGET_PER_SLICE 4u

enum
{
    RV_SYSCALL_YIELD = 0,
    RV_SYSCALL_SLEEP = 1,
    RV_SYSCALL_WRITE = 2,
    RV_SYSCALL_EXIT = 3,
};

typedef struct
{
    uint32_t syscall_number;
    uint32_t argument0;
    uint32_t argument1;
    uint32_t exit_code;
} rv_demo_context_t;

static rv32_cpu_t cpu;
static uint8_t guest_memory[RV_DEMO_MEMORY_SIZE];
static rv_demo_context_t context;

static const char success_message[] PROGMEM = "rv32 C guest exit=42";
static const char failure_message[] PROGMEM = "rv32 demo fault";

static rv32_status_t handle_ecall(struct rv32_cpu *guest_cpu,
                                  void *raw_context)
{
    rv_demo_context_t *context = raw_context;
    context->syscall_number = guest_cpu->x[17]; /* a7 */
    context->argument0 = guest_cpu->x[10];      /* a0 */
    context->argument1 = guest_cpu->x[11];      /* a1 */

    if (context->syscall_number <= RV_SYSCALL_WRITE)
    {
        return RV32_PAUSED;
    }
    if (context->syscall_number == RV_SYSCALL_EXIT)
    {
        context->exit_code = context->argument0;
        return RV32_HALTED;
    }
    return RV32_FAULT_ILLEGAL_INSTRUCTION;
}

static void process_guest_syscall(rv_demo_context_t *context)
{
    switch (context->syscall_number)
    {
    case RV_SYSCALL_YIELD:
        cpu.x[10] = 0u;
        cpu.status = RV32_RUNNING;
        task_yield();
        break;

    case RV_SYSCALL_SLEEP:
        cpu.x[10] = 0u;
        cpu.status = RV32_RUNNING;
        task_sleep(context->argument0);
        break;

    case RV_SYSCALL_WRITE:
        if (context->argument0 > RV_DEMO_MEMORY_SIZE ||
            context->argument1 > UINT8_MAX ||
            context->argument1 >
                (uint32_t)RV_DEMO_MEMORY_SIZE - context->argument0)
        {
            cpu.status = RV32_FAULT_LOAD;
            break;
        }
        uart_write_binary_frame(
            UART_FRAME_SHELL_TEXT,
            &guest_memory[(uint16_t)context->argument0],
            (uint8_t)context->argument1);
        cpu.x[10] = context->argument1;
        cpu.status = RV32_RUNNING;
        break;

    default:
        cpu.status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        break;
    }
}

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
    context.syscall_number = 0u;
    context.argument0 = 0u;
    context.argument1 = 0u;
    context.exit_code = 0u;
    rv32_init(&cpu, guest_memory, sizeof(guest_memory));
    cpu.ecall_handler = handle_ecall;
    cpu.ecall_context = &context;

    while (cpu.status == RV32_RUNNING)
    {
        rv32_run(&cpu, RV_DEMO_BUDGET_PER_SLICE);
        if (cpu.status == RV32_PAUSED)
        {
            process_guest_syscall(&context);
        }
        else if (cpu.status == RV32_RUNNING)
        {
            task_yield();
        }
    }

    if (cpu.status == RV32_HALTED && context.exit_code == 42u)
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
