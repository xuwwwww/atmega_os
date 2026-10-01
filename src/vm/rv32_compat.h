#ifndef RV32_COMPAT_H
#define RV32_COMPAT_H

#include <stdint.h>

uint32_t rv32_compat_lsr(uint32_t value, uint8_t amount);
uint32_t rv32_compat_reg_read(const volatile uint32_t *registers,
                              uint8_t index);
void rv32_compat_reg_write(volatile uint32_t *registers, uint8_t index,
                           uint32_t value);
void rv32_compat_zero_x0(volatile uint32_t *registers);

#endif
