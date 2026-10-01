#include "rv32i.h"
#include "rv32_compat.h"

#include <stddef.h>

#define RV32_OPCODE_LOAD 0x03u
#define RV32_OPCODE_FENCE 0x0fu
#define RV32_OPCODE_OP_IMM 0x13u
#define RV32_OPCODE_AUIPC 0x17u
#define RV32_OPCODE_STORE 0x23u
#define RV32_OPCODE_OP 0x33u
#define RV32_OPCODE_LUI 0x37u
#define RV32_OPCODE_BRANCH 0x63u
#define RV32_OPCODE_JALR 0x67u
#define RV32_OPCODE_JAL 0x6fu
#define RV32_OPCODE_SYSTEM 0x73u

#define REG_READ(cpu, index) \
    rv32_compat_reg_read((cpu)->x, (index))
#define REG_WRITE(cpu, index, value) \
    rv32_compat_reg_write((cpu)->x, (index), (value))

static uint32_t sign_extend(uint32_t value, uint8_t bit_count)
{
    uint32_t sign = 1UL << (bit_count - 1u);
    uint32_t mask = (sign << 1u) - 1u;

    value &= mask;
    return (value & sign) != 0u ? value | ~mask : value;
}

static uint8_t signed_less_than(uint32_t left, uint32_t right)
{
    return (left ^ 0x80000000UL) < (right ^ 0x80000000UL);
}

static uint32_t arithmetic_shift_right(uint32_t value, uint8_t amount)
{
    if (amount == 0u)
    {
        return value;
    }
    if ((value & 0x80000000UL) == 0u)
    {
        return rv32_compat_lsr(value, amount);
    }
    return rv32_compat_lsr(value, amount) |
           ~rv32_compat_lsr(0xffffffffUL, amount);
}

static uint8_t address_valid(const rv32_cpu_t *cpu, uint32_t address,
                             uint8_t width)
{
    return cpu->memory != NULL && address <= cpu->memory_size &&
           (uint32_t)cpu->memory_size - address >= width;
}

static uint16_t load_u16(const uint8_t *memory, uint32_t address)
{
    return (uint16_t)memory[address] |
           ((uint16_t)memory[address + 1u] << 8u);
}

static uint32_t load_u32(const uint8_t *memory, uint32_t address)
{
    return (uint32_t)memory[address] |
           ((uint32_t)memory[address + 1u] << 8u) |
           ((uint32_t)memory[address + 2u] << 16u) |
           ((uint32_t)memory[address + 3u] << 24u);
}

static void store_u16(uint8_t *memory, uint32_t address, uint16_t value)
{
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8u);
}

static void store_u32(uint8_t *memory, uint32_t address, uint32_t value)
{
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8u);
    memory[address + 2u] = (uint8_t)(value >> 16u);
    memory[address + 3u] = (uint8_t)(value >> 24u);
}

static uint8_t target_aligned(uint32_t target)
{
    return (target & 3u) == 0u;
}

void rv32_init(rv32_cpu_t *cpu, uint8_t *memory, uint16_t memory_size)
{
    for (uint8_t i = 0u; i < 32u; i++)
    {
        REG_WRITE(cpu, i, 0u);
    }

    cpu->pc = 0u;
    cpu->memory = memory;
    cpu->memory_size = memory_size;
    cpu->ecall_handler = NULL;
    cpu->ecall_context = NULL;
    cpu->status = RV32_RUNNING;
}

uint32_t rv32_register_read(const rv32_cpu_t *cpu, uint8_t index)
{
    return index < 32u ? REG_READ(cpu, index) : 0u;
}

void rv32_register_write(rv32_cpu_t *cpu, uint8_t index, uint32_t value)
{
    if (index < 32u)
    {
        REG_WRITE(cpu, index, value);
    }
}

rv32_status_t rv32_step(rv32_cpu_t *cpu)
{
    uint32_t instruction;
    uint32_t next_pc;
    uint8_t opcode;
    uint8_t rd;
    uint8_t funct3;
    uint8_t rs1;
    uint8_t rs2;
    uint8_t funct7;

    if (cpu == NULL || cpu->status != RV32_RUNNING)
    {
        return cpu == NULL ? RV32_FAULT_FETCH : cpu->status;
    }
    if (!target_aligned(cpu->pc))
    {
        cpu->status = RV32_FAULT_MISALIGNED;
        return cpu->status;
    }
    if (!address_valid(cpu, cpu->pc, 4u))
    {
        cpu->status = RV32_FAULT_FETCH;
        return cpu->status;
    }
    instruction = load_u32(cpu->memory, cpu->pc);
    next_pc = cpu->pc + 4u;
    opcode = (uint8_t)(instruction & 0x7fu);
    rd = (uint8_t)((instruction >> 7u) & 0x1fu);
    funct3 = (uint8_t)((instruction >> 12u) & 7u);
    rs1 = (uint8_t)((instruction >> 15u) & 0x1fu);
    rs2 = (uint8_t)((instruction >> 20u) & 0x1fu);
    funct7 = (uint8_t)(instruction >> 25u);

    switch (opcode)
    {
    case RV32_OPCODE_LUI:
        REG_WRITE(cpu, rd, instruction & 0xfffff000UL);
        break;
    case RV32_OPCODE_AUIPC:
        REG_WRITE(cpu, rd, cpu->pc + (instruction & 0xfffff000UL));
        break;

    case RV32_OPCODE_OP_IMM:
    {
        uint32_t immediate = sign_extend(instruction >> 20u, 12u);
        uint8_t shift = (uint8_t)((instruction >> 20u) & 0x1fu);

        switch (funct3)
        {
        case 0u: REG_WRITE(cpu, rd, REG_READ(cpu, rs1) + immediate); break;
        case 1u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) << shift);
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 2u:
            REG_WRITE(cpu, rd, signed_less_than(REG_READ(cpu, rs1), immediate));
            break;
        case 3u: REG_WRITE(cpu, rd, REG_READ(cpu, rs1) < immediate); break;
        case 4u: REG_WRITE(cpu, rd, REG_READ(cpu, rs1) ^ immediate); break;
        case 5u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd,
                          rv32_compat_lsr(REG_READ(cpu, rs1), shift));
            else if (funct7 == 0x20u)
                REG_WRITE(cpu, rd,
                          arithmetic_shift_right(REG_READ(cpu, rs1), shift));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 6u: REG_WRITE(cpu, rd, REG_READ(cpu, rs1) | immediate); break;
        case 7u: REG_WRITE(cpu, rd, REG_READ(cpu, rs1) & immediate); break;
        default: cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION; break;
        }
        break;
    }

    case RV32_OPCODE_OP:
    {
        uint8_t shift = (uint8_t)(REG_READ(cpu, rs2) & 0x1fu);

        switch (funct3)
        {
        case 0u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) + REG_READ(cpu, rs2));
            else if (funct7 == 0x20u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) - REG_READ(cpu, rs2));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 1u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) << shift);
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 2u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd,
                          signed_less_than(REG_READ(cpu, rs1),
                                           REG_READ(cpu, rs2)));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 3u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) < REG_READ(cpu, rs2));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 4u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) ^ REG_READ(cpu, rs2));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 5u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd,
                          rv32_compat_lsr(REG_READ(cpu, rs1), shift));
            else if (funct7 == 0x20u)
                REG_WRITE(cpu, rd,
                          arithmetic_shift_right(REG_READ(cpu, rs1), shift));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 6u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) | REG_READ(cpu, rs2));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        case 7u:
            if (funct7 == 0u)
                REG_WRITE(cpu, rd, REG_READ(cpu, rs1) & REG_READ(cpu, rs2));
            else cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        default: cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION; break;
        }
        break;
    }

    case RV32_OPCODE_LOAD:
    {
        uint32_t address = REG_READ(cpu, rs1) +
                           sign_extend(instruction >> 20u, 12u);
        uint8_t width;

        if (funct3 == 0u || funct3 == 4u) width = 1u;
        else if (funct3 == 1u || funct3 == 5u) width = 2u;
        else if (funct3 == 2u) width = 4u;
        else
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }

        if ((address & (uint32_t)(width - 1u)) != 0u)
            cpu->status = RV32_FAULT_MISALIGNED;
        else if (!address_valid(cpu, address, width))
            cpu->status = RV32_FAULT_LOAD;
        else if (funct3 == 0u)
            REG_WRITE(cpu, rd, sign_extend(cpu->memory[address], 8u));
        else if (funct3 == 1u)
            REG_WRITE(cpu, rd,
                      sign_extend(load_u16(cpu->memory, address), 16u));
        else if (funct3 == 2u)
            REG_WRITE(cpu, rd, load_u32(cpu->memory, address));
        else if (funct3 == 4u)
            REG_WRITE(cpu, rd, cpu->memory[address]);
        else
            REG_WRITE(cpu, rd, load_u16(cpu->memory, address));
        break;
    }

    case RV32_OPCODE_STORE:
    {
        uint32_t immediate = ((instruction >> 7u) & 0x1fu) |
                             (((instruction >> 25u) & 0x7fu) << 5u);
        uint32_t address = REG_READ(cpu, rs1) + sign_extend(immediate, 12u);
        uint8_t width;

        if (funct3 == 0u) width = 1u;
        else if (funct3 == 1u) width = 2u;
        else if (funct3 == 2u) width = 4u;
        else
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }

        if ((address & (uint32_t)(width - 1u)) != 0u)
            cpu->status = RV32_FAULT_MISALIGNED;
        else if (!address_valid(cpu, address, width))
            cpu->status = RV32_FAULT_STORE;
        else if (width == 1u)
            cpu->memory[address] = (uint8_t)REG_READ(cpu, rs2);
        else if (width == 2u)
            store_u16(cpu->memory, address, (uint16_t)REG_READ(cpu, rs2));
        else
            store_u32(cpu->memory, address, REG_READ(cpu, rs2));
        break;
    }

    case RV32_OPCODE_BRANCH:
    {
        uint32_t encoded = (((instruction >> 31u) & 1u) << 12u) |
                           (((instruction >> 7u) & 1u) << 11u) |
                           (((instruction >> 25u) & 0x3fu) << 5u) |
                           (((instruction >> 8u) & 0x0fu) << 1u);
        uint32_t target = cpu->pc + sign_extend(encoded, 13u);
        uint8_t taken = 0u;

        switch (funct3)
        {
        case 0u: taken = REG_READ(cpu, rs1) == REG_READ(cpu, rs2); break;
        case 1u: taken = REG_READ(cpu, rs1) != REG_READ(cpu, rs2); break;
        case 4u:
            taken = signed_less_than(REG_READ(cpu, rs1), REG_READ(cpu, rs2));
            break;
        case 5u:
            taken = !signed_less_than(REG_READ(cpu, rs1), REG_READ(cpu, rs2));
            break;
        case 6u: taken = REG_READ(cpu, rs1) < REG_READ(cpu, rs2); break;
        case 7u: taken = REG_READ(cpu, rs1) >= REG_READ(cpu, rs2); break;
        default: cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION; break;
        }

        if (cpu->status == RV32_RUNNING && taken)
        {
            if (!target_aligned(target)) cpu->status = RV32_FAULT_MISALIGNED;
            else next_pc = target;
        }
        break;
    }

    case RV32_OPCODE_JAL:
    {
        uint32_t encoded = (((instruction >> 31u) & 1u) << 20u) |
                           (((instruction >> 12u) & 0xffu) << 12u) |
                           (((instruction >> 20u) & 1u) << 11u) |
                           (((instruction >> 21u) & 0x3ffu) << 1u);
        uint32_t target = cpu->pc + sign_extend(encoded, 21u);

        if (!target_aligned(target)) cpu->status = RV32_FAULT_MISALIGNED;
        else
        {
            REG_WRITE(cpu, rd, next_pc);
            next_pc = target;
        }
        break;
    }

    case RV32_OPCODE_JALR:
    {
        uint32_t target = (REG_READ(cpu, rs1) +
                           sign_extend(instruction >> 20u, 12u)) & ~1UL;

        if (funct3 != 0u) cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        else if (!target_aligned(target)) cpu->status = RV32_FAULT_MISALIGNED;
        else
        {
            REG_WRITE(cpu, rd, next_pc);
            next_pc = target;
        }
        break;
    }

    case RV32_OPCODE_FENCE:
        if (funct3 != 0u) cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        break;

    case RV32_OPCODE_SYSTEM:
        if (instruction != 0x00000073UL)
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        else if (cpu->ecall_handler == NULL)
            cpu->status = RV32_HALTED;
        else
            cpu->status = cpu->ecall_handler(cpu, cpu->ecall_context);
        break;

    default:
        cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        break;
    }

    rv32_compat_zero_x0(cpu->x);
    if (cpu->status == RV32_RUNNING || cpu->status == RV32_PAUSED)
    {
        cpu->pc = next_pc;
    }
    return cpu->status;
}

rv32_status_t rv32_run(rv32_cpu_t *cpu, uint16_t instruction_budget)
{
    while (instruction_budget > 0u && cpu->status == RV32_RUNNING)
    {
        rv32_step(cpu);
        instruction_budget--;
    }
    return cpu->status;
}
