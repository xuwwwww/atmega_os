#ifndef UART_H
#define UART_H

#include "../kernel/scheduler.h"

#include <stdint.h>

#define UART_FRAME_STACK_REPORT 0x02u
#define UART_FRAME_SHELL_TEXT 0x03u
#define UART_FRAME_PANIC 0x04u

void uart_init(void);
void uart_write_char(char c);
void uart_write_string(const char *text);
void uart_write_compressed_P(const uint8_t *data, uint8_t byte_count,
                             uint16_t bit_count);
void uart_write_binary_frame(uint8_t frame_type, const uint8_t *data,
                             uint8_t byte_count);
void uart_write_binary_frame_P(uint8_t frame_type, const uint8_t *data,
                               uint8_t byte_count);
uint8_t uart_read_char(char *out);
void uart_read_char_wait(char *out);
wait_result_t uart_read_char_timeout(char *out, uint32_t timeout_ms);
uint8_t uart_rx_overflowed(void);
void uart_panic_write(uint8_t code, uint8_t detail);

#endif
