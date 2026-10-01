#include "rv32_compat.h"

uint32_t rv32_compat_lsr(uint32_t value, uint8_t amount)
{
    uint8_t *byte = (uint8_t *)&value;

    while (amount >= 8u)
    {
        byte[0] = byte[1];
        byte[1] = byte[2];
        byte[2] = byte[3];
        byte[3] = 0u;
        amount -= 8u;
    }
    while (amount-- != 0u)
    {
        byte[0] = (uint8_t)((byte[0] >> 1u) | (byte[1] << 7u));
        byte[1] = (uint8_t)((byte[1] >> 1u) | (byte[2] << 7u));
        byte[2] = (uint8_t)((byte[2] >> 1u) | (byte[3] << 7u));
        byte[3] >>= 1u;
    }
    return value;
}

uint32_t rv32_compat_reg_read(const volatile uint32_t *registers,
                              uint8_t index)
{
    const volatile uint8_t *byte =
        (const volatile uint8_t *)registers + (uint16_t)index * 4u;

    return (uint32_t)byte[0] |
           ((uint32_t)byte[1] << 8u) |
           ((uint32_t)byte[2] << 16u) |
           ((uint32_t)byte[3] << 24u);
}

void rv32_compat_reg_write(volatile uint32_t *registers, uint8_t index,
                           uint32_t value)
{
    volatile uint8_t *byte =
        (volatile uint8_t *)registers + (uint16_t)index * 4u;

    byte[0] = (uint8_t)value;
    byte[1] = (uint8_t)(value >> 8u);
    byte[2] = (uint8_t)(value >> 16u);
    byte[3] = (uint8_t)(value >> 24u);
}

void rv32_compat_zero_x0(volatile uint32_t *registers)
{
    rv32_compat_reg_write(registers, 0u, 0u);
}
