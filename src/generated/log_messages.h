#ifndef GENERATED_LOG_MESSAGES_H
#define GENERATED_LOG_MESSAGES_H

#include <avr/pgmspace.h>
#include <stdint.h>

extern const uint8_t log_boot_data[16] PROGMEM;
#define LOG_BOOT_BYTE_COUNT 16u
#define LOG_BOOT_BIT_COUNT 125u

extern const uint8_t log_shell_ready_data[16] PROGMEM;
#define LOG_SHELL_READY_BYTE_COUNT 16u
#define LOG_SHELL_READY_BIT_COUNT 121u

extern const uint8_t log_shell_prompt_data[3] PROGMEM;
#define LOG_SHELL_PROMPT_BYTE_COUNT 3u
#define LOG_SHELL_PROMPT_BIT_COUNT 19u

extern const uint8_t log_shell_help_data[140] PROGMEM;
#define LOG_SHELL_HELP_BYTE_COUNT 140u
#define LOG_SHELL_HELP_BIT_COUNT 1116u

extern const uint8_t log_shell_ok_data[3] PROGMEM;
#define LOG_SHELL_OK_BYTE_COUNT 3u
#define LOG_SHELL_OK_BIT_COUNT 19u

extern const uint8_t log_shell_unknown_data[19] PROGMEM;
#define LOG_SHELL_UNKNOWN_BYTE_COUNT 19u
#define LOG_SHELL_UNKNOWN_BIT_COUNT 146u

extern const uint8_t log_shell_line_long_data[13] PROGMEM;
#define LOG_SHELL_LINE_LONG_BYTE_COUNT 13u
#define LOG_SHELL_LINE_LONG_BIT_COUNT 99u

extern const uint8_t log_shell_restart_usage_data[17] PROGMEM;
#define LOG_SHELL_RESTART_USAGE_BYTE_COUNT 17u
#define LOG_SHELL_RESTART_USAGE_BIT_COUNT 131u

extern const uint8_t log_shell_restart_failed_data[26] PROGMEM;
#define LOG_SHELL_RESTART_FAILED_BYTE_COUNT 26u
#define LOG_SHELL_RESTART_FAILED_BIT_COUNT 205u

#endif
