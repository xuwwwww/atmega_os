#include "rv32i.h"

#include <stddef.h>

#define RV32_OPCODE_LOAD 0x03u
#define RV32_OPCODE_OP_IMM 0x13u
#define RV32_OPCODE_STORE 0x23u
#define RV32_OPCODE_OP 0x33u
#define RV32_OPCODE_LUI 0x37u
#define RV32_OPCODE_BRANCH 0x63u
#define RV32_OPCODE_JALR 0x67u
#define RV32_OPCODE_JAL 0x6fu
#define RV32_OPCODE_SYSTEM 0x73u

static int32_t sign_extend(uint32_t value, uint8_t bit_count)
{
    uint32_t sign = 1UL << (bit_count - 1u);

    return (int32_t)((value ^ sign) - sign);
}

static uint8_t address_valid(const rv32_cpu_t *cpu, uint32_t address)
{
    return cpu->memory != NULL && address <= cpu->memory_size &&
           (uint32_t)cpu->memory_size - address >= 4u;
}

static uint32_t load_u32(const uint8_t *memory, uint32_t address)
{
    return (uint32_t)memory[address] |
           ((uint32_t)memory[address + 1u] << 8u) |
           ((uint32_t)memory[address + 2u] << 16u) |
           ((uint32_t)memory[address + 3u] << 24u);
}

static void store_u32(uint8_t *memory, uint32_t address, uint32_t value)
{
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8u);
    memory[address + 2u] = (uint8_t)(value >> 16u);
    memory[address + 3u] = (uint8_t)(value >> 24u);
}

static rv32_status_t set_pc(rv32_cpu_t *cpu, uint32_t next_pc)
{
    if ((next_pc & 3u) != 0u)
    {
        return RV32_FAULT_MISALIGNED;
    }

    cpu->pc = next_pc;
    return RV32_RUNNING;
}

void rv32_init(rv32_cpu_t *cpu, uint8_t *memory, uint16_t memory_size)
{
    for (uint8_t i = 0u; i < 32u; i++)
    {
        cpu->x[i] = 0u;
    }

    cpu->pc = 0u;
    cpu->memory = memory;
    cpu->memory_size = memory_size;
    cpu->ecall_handler = NULL;
    cpu->ecall_context = NULL;
    cpu->status = RV32_RUNNING;
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

    if (cpu == NULL || cpu->status != RV32_RUNNING)
    {
        return cpu == NULL ? RV32_FAULT_FETCH : cpu->status;
    }

    if ((cpu->pc & 3u) != 0u)
    {
        cpu->status = RV32_FAULT_MISALIGNED;
        return cpu->status;
    }
    if (!address_valid(cpu, cpu->pc))
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

    switch (opcode)
    {
    case RV32_OPCODE_LUI:
        cpu->x[rd] = instruction & 0xfffff000UL;
        break;

    case RV32_OPCODE_OP_IMM:
        if (funct3 != 0u)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }
        cpu->x[rd] = cpu->x[rs1] +
                     (uint32_t)sign_extend(instruction >> 20u, 12u);
        break;

    case RV32_OPCODE_OP:
        if (funct3 != 0u)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }
        if ((instruction >> 25u) == 0u)
        {
            cpu->x[rd] = cpu->x[rs1] + cpu->x[rs2];
        }
        else if ((instruction >> 25u) == 0x20u)
        {
            cpu->x[rd] = cpu->x[rs1] - cpu->x[rs2];
        }
        else
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        }
        break;

    case RV32_OPCODE_LOAD:
    {
        uint32_t address;

        if (funct3 != 2u)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }
        address = cpu->x[rs1] +
                  (uint32_t)sign_extend(instruction >> 20u, 12u);
        if ((address & 3u) != 0u)
        {
            cpu->status = RV32_FAULT_MISALIGNED;
        }
        else if (!address_valid(cpu, address))
        {
            cpu->status = RV32_FAULT_LOAD;
        }
        else
        {
            cpu->x[rd] = load_u32(cpu->memory, address);
        }
        break;
    }

    case RV32_OPCODE_STORE:
    {
        uint32_t immediate = ((instruction >> 7u) & 0x1fu) |
                             (((instruction >> 25u) & 0x7fu) << 5u);
        uint32_t address;

        if (funct3 != 2u)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }
        address = cpu->x[rs1] +
                  (uint32_t)sign_extend(immediate, 12u);
        if ((address & 3u) != 0u)
        {
            cpu->status = RV32_FAULT_MISALIGNED;
        }
        else if (!address_valid(cpu, address))
        {
            cpu->status = RV32_FAULT_STORE;
        }
        else
        {
            store_u32(cpu->memory, address, cpu->x[rs2]);
        }
        break;
    }

    case RV32_OPCODE_BRANCH:
    {
        uint32_t encoded = (((instruction >> 31u) & 1u) << 12u) |
                           (((instruction >> 7u) & 1u) << 11u) |
                           (((instruction >> 25u) & 0x3fu) << 5u) |
                           (((instruction >> 8u) & 0x0fu) << 1u);
        uint8_t taken;

        if (funct3 == 0u)
        {
            taken = cpu->x[rs1] == cpu->x[rs2];
        }
        else if (funct3 == 1u)
        {
            taken = cpu->x[rs1] != cpu->x[rs2];
        }
        else
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }

        if (taken)
        {
            cpu->status = set_pc(
                cpu, cpu->pc + (uint32_t)sign_extend(encoded, 13u));
            next_pc = cpu->pc;
        }
        break;
    }

    case RV32_OPCODE_JAL:
    {
        uint32_t encoded = (((instruction >> 31u) & 1u) << 20u) |
                           (((instruction >> 12u) & 0xffu) << 12u) |
                           (((instruction >> 20u) & 1u) << 11u) |
                           (((instruction >> 21u) & 0x3ffu) << 1u);
        uint32_t target = cpu->pc +
                          (uint32_t)sign_extend(encoded, 21u);

        if ((target & 3u) != 0u)
        {
            cpu->status = RV32_FAULT_MISALIGNED;
            break;
        }
        cpu->x[rd] = next_pc;
        next_pc = target;
        break;
    }

    case RV32_OPCODE_JALR:
    {
        uint32_t target;

        if (funct3 != 0u)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
            break;
        }
        target = (cpu->x[rs1] +
                  (uint32_t)sign_extend(instruction >> 20u, 12u)) & ~1UL;
        if ((target & 3u) != 0u)
        {
            cpu->status = RV32_FAULT_MISALIGNED;
            break;
        }
        cpu->x[rd] = next_pc;
        next_pc = target;
        break;
    }

    case RV32_OPCODE_SYSTEM:
        if (instruction != 0x00000073UL)
        {
            cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        }
        else if (cpu->ecall_handler == NULL)
        {
            cpu->status = RV32_HALTED;
        }
        else
        {
            cpu->status = cpu->ecall_handler(cpu, cpu->ecall_context);
        }
        break;

    default:
        cpu->status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        break;
    }

    cpu->x[0] = 0u;
    if (cpu->status == RV32_RUNNING)
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
