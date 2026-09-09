#include "rv32i.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPCODE_LOAD 0x03u
#define OPCODE_OP_IMM 0x13u
#define OPCODE_STORE 0x23u
#define OPCODE_OP 0x33u
#define OPCODE_LUI 0x37u
#define OPCODE_BRANCH 0x63u
#define OPCODE_JALR 0x67u
#define OPCODE_JAL 0x6fu

static void emit(uint8_t *memory, uint16_t address, uint32_t instruction)
{
    memory[address] = (uint8_t)instruction;
    memory[address + 1u] = (uint8_t)(instruction >> 8u);
    memory[address + 2u] = (uint8_t)(instruction >> 16u);
    memory[address + 3u] = (uint8_t)(instruction >> 24u);
}

static uint32_t encode_i(int16_t imm, uint8_t rs1, uint8_t funct3,
                         uint8_t rd, uint8_t opcode)
{
    return (((uint32_t)imm & 0xfffu) << 20u) |
           ((uint32_t)rs1 << 15u) | ((uint32_t)funct3 << 12u) |
           ((uint32_t)rd << 7u) | opcode;
}

static uint32_t encode_r(uint8_t funct7, uint8_t rs2, uint8_t rs1,
                         uint8_t funct3, uint8_t rd)
{
    return ((uint32_t)funct7 << 25u) | ((uint32_t)rs2 << 20u) |
           ((uint32_t)rs1 << 15u) | ((uint32_t)funct3 << 12u) |
           ((uint32_t)rd << 7u) | OPCODE_OP;
}

static uint32_t encode_s(int16_t imm, uint8_t rs2, uint8_t rs1,
                         uint8_t funct3)
{
    uint32_t encoded = (uint32_t)imm & 0xfffu;

    return ((encoded >> 5u) << 25u) | ((uint32_t)rs2 << 20u) |
           ((uint32_t)rs1 << 15u) | ((uint32_t)funct3 << 12u) |
           ((encoded & 0x1fu) << 7u) | OPCODE_STORE;
}

static uint32_t encode_b(int16_t imm, uint8_t rs2, uint8_t rs1,
                         uint8_t funct3)
{
    uint32_t encoded = (uint32_t)imm & 0x1fffu;

    return (((encoded >> 12u) & 1u) << 31u) |
           (((encoded >> 5u) & 0x3fu) << 25u) |
           ((uint32_t)rs2 << 20u) | ((uint32_t)rs1 << 15u) |
           ((uint32_t)funct3 << 12u) |
           (((encoded >> 1u) & 0x0fu) << 8u) |
           (((encoded >> 11u) & 1u) << 7u) | OPCODE_BRANCH;
}

static uint32_t encode_j(int32_t imm, uint8_t rd)
{
    uint32_t encoded = (uint32_t)imm & 0x1fffffu;

    return (((encoded >> 20u) & 1u) << 31u) |
           (((encoded >> 1u) & 0x3ffu) << 21u) |
           (((encoded >> 11u) & 1u) << 20u) |
           (((encoded >> 12u) & 0xffu) << 12u) |
           ((uint32_t)rd << 7u) | OPCODE_JAL;
}

static void test_arithmetic_and_halt(void)
{
    uint8_t memory[64] = {0};
    rv32_cpu_t cpu;

    emit(memory, 0u, encode_i(7, 0u, 0u, 1u, OPCODE_OP_IMM));
    emit(memory, 4u, encode_i(-2, 0u, 0u, 2u, OPCODE_OP_IMM));
    emit(memory, 8u, encode_r(0u, 2u, 1u, 0u, 3u));
    emit(memory, 12u, encode_r(0x20u, 2u, 1u, 0u, 4u));
    emit(memory, 16u, encode_i(99, 0u, 0u, 0u, OPCODE_OP_IMM));
    emit(memory, 20u, 0x12345000u | (5u << 7u) | OPCODE_LUI);
    emit(memory, 24u, 0x00000073u);

    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_run(&cpu, 20u) == RV32_HALTED);
    assert(cpu.x[0] == 0u);
    assert(cpu.x[1] == 7u);
    assert(cpu.x[2] == 0xfffffffeUL);
    assert(cpu.x[3] == 5u);
    assert(cpu.x[4] == 9u);
    assert(cpu.x[5] == 0x12345000UL);
    assert(cpu.pc == 24u);
}

static void test_memory_and_control_flow(void)
{
    uint8_t memory[128] = {0};
    rv32_cpu_t cpu;

    emit(memory, 0u, encode_i(80, 0u, 0u, 1u, OPCODE_OP_IMM));
    emit(memory, 4u, encode_i(42, 0u, 0u, 2u, OPCODE_OP_IMM));
    emit(memory, 8u, encode_s(0, 2u, 1u, 2u));
    emit(memory, 12u, encode_i(0, 1u, 2u, 3u, OPCODE_LOAD));
    emit(memory, 16u, encode_b(8, 3u, 2u, 0u));
    emit(memory, 20u, encode_i(1, 0u, 0u, 4u, OPCODE_OP_IMM));
    emit(memory, 24u, encode_j(8, 5u));
    emit(memory, 28u, encode_i(2, 0u, 0u, 4u, OPCODE_OP_IMM));
    emit(memory, 32u, encode_i(44, 0u, 0u, 6u, OPCODE_OP_IMM));
    emit(memory, 36u, encode_i(0, 6u, 0u, 7u, OPCODE_JALR));
    emit(memory, 40u, encode_i(3, 0u, 0u, 4u, OPCODE_OP_IMM));
    emit(memory, 44u, 0x00000073u);

    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_run(&cpu, 30u) == RV32_HALTED);
    assert(cpu.x[3] == 42u);
    assert(cpu.x[4] == 0u);
    assert(cpu.x[5] == 28u);
    assert(cpu.x[7] == 40u);
    assert(cpu.pc == 44u);
}

static void test_faults_and_budget(void)
{
    uint8_t memory[16] = {0};
    rv32_cpu_t cpu;

    emit(memory, 0u, encode_j(0, 0u));
    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_run(&cpu, 5u) == RV32_RUNNING);
    assert(cpu.pc == 0u);

    cpu.pc = 2u;
    assert(rv32_step(&cpu) == RV32_FAULT_MISALIGNED);

    memset(memory, 0, sizeof(memory));
    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_step(&cpu) == RV32_FAULT_ILLEGAL_INSTRUCTION);

    emit(memory, 0u, encode_i(2, 0u, 2u, 1u, OPCODE_LOAD));
    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_step(&cpu) == RV32_FAULT_MISALIGNED);

    emit(memory, 0u, encode_i(16, 0u, 2u, 1u, OPCODE_LOAD));
    rv32_init(&cpu, memory, sizeof(memory));
    assert(rv32_step(&cpu) == RV32_FAULT_LOAD);
}

int main(void)
{
    test_arithmetic_and_halt();
    test_memory_and_control_flow();
    test_faults_and_budget();
    puts("RV32I emulator tests passed");
    return 0;
}
