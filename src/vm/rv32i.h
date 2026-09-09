#ifndef RV32I_H
#define RV32I_H

#include <stdint.h>

typedef enum
{
    RV32_RUNNING = 0,
    RV32_PAUSED,
    RV32_HALTED,
    RV32_FAULT_FETCH,
    RV32_FAULT_LOAD,
    RV32_FAULT_STORE,
    RV32_FAULT_MISALIGNED,
    RV32_FAULT_ILLEGAL_INSTRUCTION,
} rv32_status_t;

struct rv32_cpu;

typedef rv32_status_t (*rv32_ecall_handler_t)(struct rv32_cpu *cpu,
                                               void *context);

typedef struct rv32_cpu
{
    uint32_t x[32];
    uint32_t pc;
    uint8_t *memory;
    uint16_t memory_size;
    rv32_ecall_handler_t ecall_handler;
    void *ecall_context;
    rv32_status_t status;
} rv32_cpu_t;

void rv32_init(rv32_cpu_t *cpu, uint8_t *memory, uint16_t memory_size);
rv32_status_t rv32_step(rv32_cpu_t *cpu);
rv32_status_t rv32_run(rv32_cpu_t *cpu, uint16_t instruction_budget);

#endif
