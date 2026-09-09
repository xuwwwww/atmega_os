#include "shell_task.h"

#include "../drivers/uart.h"
#include "../generated/log_messages.h"
#include "../kernel/panic.h"
#include "../kernel/scheduler.h"
#include "../kernel/timer.h"
#include "led_task.h"

#include <avr/pgmspace.h>
#include <stdint.h>
#include <string.h>

#define SHELL_LINE_CAPACITY 64u

static const char command_help[] PROGMEM = "help";
static const char command_uptime[] PROGMEM = "uptime";
static const char command_tasks[] PROGMEM = "tasks";
static const char command_stack[] PROGMEM = "stack";
static const char command_faults[] PROGMEM = "faults";
static const char command_panic[] PROGMEM = "panic";
static const char command_led_on[] PROGMEM = "led on";
static const char command_led_off[] PROGMEM = "led off";
static const char command_led_blink[] PROGMEM = "led blink";
static const char command_restart_prefix[] PROGMEM = "restart ";

static void shell_send_compressed(const uint8_t *data, uint8_t byte_count,
                                  uint16_t bit_count)
{
    uart_write_compressed_P(data, byte_count, bit_count);
}

static void shell_send_text(const char *text, uint8_t length)
{
    uart_write_binary_frame(UART_FRAME_SHELL_TEXT,
                            (const uint8_t *)text, length);
}

static void buffer_append_char(char *buffer, uint8_t *length, char value)
{
    if (*length < SHELL_LINE_CAPACITY - 1u)
    {
        buffer[*length] = value;
        (*length)++;
    }
}

static void buffer_append_P(char *buffer, uint8_t *length,
                            const char *text)
{
    char value;

    while ((value = (char)pgm_read_byte(text++)) != '\0')
    {
        buffer_append_char(buffer, length, value);
    }
}

static void buffer_append_u32(char *buffer, uint8_t *length, uint32_t value)
{
    char reversed[10];
    uint8_t digits = 0u;

    do
    {
        reversed[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (digits > 0u)
    {
        buffer_append_char(buffer, length, reversed[--digits]);
    }
}

static void shell_send_uptime(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("uptime_ms="));
    buffer_append_u32(buffer, &length, timer_millis());
    shell_send_text(buffer, length);
}

static void shell_send_tasks(char *buffer)
{
    for (uint8_t i = 0u; i < scheduler_task_count(); i++)
    {
        uint8_t length = 0u;

        buffer_append_P(buffer, &length, PSTR("task="));
        buffer_append_u32(buffer, &length, i);
        buffer_append_P(buffer, &length, PSTR(" state="));
        buffer_append_u32(buffer, &length, scheduler_task_state(i));
        buffer_append_P(buffer, &length, PSTR(" priority="));
        buffer_append_u32(buffer, &length,
                          scheduler_task_base_priority(i));
        buffer_append_char(buffer, &length, '/');
        buffer_append_u32(buffer, &length,
                          scheduler_task_effective_priority(i));
        shell_send_text(buffer, length);
    }
}

static void shell_send_stacks(char *buffer)
{
    for (uint8_t i = 0u; i < scheduler_task_count(); i++)
    {
        uint8_t length = 0u;
        uint8_t size = scheduler_task_stack_size(i);
        uint8_t unused = scheduler_task_stack_unused(i);

        buffer_append_P(buffer, &length, PSTR("task="));
        buffer_append_u32(buffer, &length, i);
        buffer_append_P(buffer, &length, PSTR(" stack="));
        buffer_append_u32(buffer, &length, size);
        buffer_append_P(buffer, &length, PSTR(" used="));
        buffer_append_u32(buffer, &length, (uint8_t)(size - unused));
        buffer_append_P(buffer, &length, PSTR(" unused="));
        buffer_append_u32(buffer, &length, unused);
        shell_send_text(buffer, length);
    }
}

static void shell_send_faults(char *buffer)
{
    uint8_t length = 0u;
    uint8_t panic_code;
    uint8_t panic_detail;

    buffer_append_P(buffer, &length, PSTR("reset_cause="));
    buffer_append_u32(buffer, &length, kernel_reset_cause());

    if (kernel_last_panic(&panic_code, &panic_detail))
    {
        buffer_append_P(buffer, &length, PSTR(" panic_code="));
        buffer_append_u32(buffer, &length, panic_code);
        buffer_append_P(buffer, &length, PSTR(" detail="));
        buffer_append_u32(buffer, &length, panic_detail);
    }
    else
    {
        buffer_append_P(buffer, &length, PSTR(" panic=none"));
    }

    shell_send_text(buffer, length);
}

static uint8_t shell_parse_task_id(const char *text, uint8_t *task_id)
{
    uint16_t value = 0u;

    if (*text == '\0')
    {
        return 0u;
    }

    while (*text != '\0')
    {
        if (*text < '0' || *text > '9')
        {
            return 0u;
        }
        value = (uint16_t)(value * 10u + (uint8_t)(*text - '0'));
        if (value >= scheduler_task_count())
        {
            return 0u;
        }
        text++;
    }

    *task_id = (uint8_t)value;
    return 1u;
}

static void shell_process_line(char *line)
{
    char *end;

    while (*line == ' ' || *line == '\t')
    {
        line++;
    }

    end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
    {
        *--end = '\0';
    }

    if (strcmp_P(line, command_help) == 0)
    {
        shell_send_compressed(log_shell_help_data,
                              LOG_SHELL_HELP_BYTE_COUNT,
                              LOG_SHELL_HELP_BIT_COUNT);
    }
    else if (strcmp_P(line, command_uptime) == 0)
    {
        shell_send_uptime(line);
    }
    else if (strcmp_P(line, command_tasks) == 0)
    {
        shell_send_tasks(line);
    }
    else if (strcmp_P(line, command_stack) == 0)
    {
        shell_send_stacks(line);
    }
    else if (strcmp_P(line, command_faults) == 0)
    {
        shell_send_faults(line);
    }
    else if (strcmp_P(line, command_panic) == 0)
    {
        kernel_panic(PANIC_MANUAL_TEST, scheduler_current_task_index());
    }
    else if (strcmp_P(line, command_led_on) == 0)
    {
        led_task_set_mode(LED_MODE_ON);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (strcmp_P(line, command_led_off) == 0)
    {
        led_task_set_mode(LED_MODE_OFF);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (strcmp_P(line, command_led_blink) == 0)
    {
        led_task_set_mode(LED_MODE_BLINK);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (strncmp_P(line, command_restart_prefix,
                       sizeof(command_restart_prefix) - 1u) == 0)
    {
        uint8_t task_id;
        const char *argument = line + sizeof(command_restart_prefix) - 1u;

        if (!shell_parse_task_id(argument, &task_id))
        {
            shell_send_compressed(log_shell_restart_usage_data,
                                  LOG_SHELL_RESTART_USAGE_BYTE_COUNT,
                                  LOG_SHELL_RESTART_USAGE_BIT_COUNT);
        }
        else if (!task_restart(task_id))
        {
            shell_send_compressed(log_shell_restart_failed_data,
                                  LOG_SHELL_RESTART_FAILED_BYTE_COUNT,
                                  LOG_SHELL_RESTART_FAILED_BIT_COUNT);
        }
        else
        {
            shell_send_compressed(log_shell_ok_data,
                                  LOG_SHELL_OK_BYTE_COUNT,
                                  LOG_SHELL_OK_BIT_COUNT);
        }
    }
    else if (line[0] != '\0')
    {
        shell_send_compressed(log_shell_unknown_data,
                              LOG_SHELL_UNKNOWN_BYTE_COUNT,
                              LOG_SHELL_UNKNOWN_BIT_COUNT);
    }
}

void shell_task(void)
{
    char line[SHELL_LINE_CAPACITY];
    uint8_t length = 0u;
    uint8_t overflow = 0u;
    uint8_t ignore_lf = 0u;

    shell_send_compressed(log_shell_ready_data,
                          LOG_SHELL_READY_BYTE_COUNT,
                          LOG_SHELL_READY_BIT_COUNT);
    shell_send_compressed(log_shell_prompt_data,
                          LOG_SHELL_PROMPT_BYTE_COUNT,
                          LOG_SHELL_PROMPT_BIT_COUNT);

    for (;;)
    {
        char input;

        uart_read_char_wait(&input);

        if (input == '\n' && ignore_lf)
        {
            ignore_lf = 0u;
            continue;
        }
        ignore_lf = 0u;

        if (input == '\r' || input == '\n')
        {
            uart_write_char('\r');
            uart_write_char('\n');

            if (overflow)
            {
                shell_send_compressed(log_shell_line_long_data,
                                      LOG_SHELL_LINE_LONG_BYTE_COUNT,
                                      LOG_SHELL_LINE_LONG_BIT_COUNT);
            }
            else
            {
                line[length] = '\0';
                shell_process_line(line);
            }
            length = 0u;
            overflow = 0u;
            ignore_lf = input == '\r';
            shell_send_compressed(log_shell_prompt_data,
                                  LOG_SHELL_PROMPT_BYTE_COUNT,
                                  LOG_SHELL_PROMPT_BIT_COUNT);
        }
        else if (input == '\b' || (uint8_t)input == 0x7fu)
        {
            if (length > 0u)
            {
                length--;
                uart_write_char('\b');
                uart_write_char(' ');
                uart_write_char('\b');
            }
        }
        else if ((uint8_t)input >= 0x20u && (uint8_t)input <= 0x7eu)
        {
            if (length < SHELL_LINE_CAPACITY - 1u)
            {
                uart_write_char(input);
                if (input >= 'A' && input <= 'Z')
                {
                    input = (char)(input + ('a' - 'A'));
                }
                line[length++] = input;
            }
            else
            {
                overflow = 1u;
                uart_write_char('\a');
            }
        }
    }
}
