#include "shell_task.h"

#include "../drivers/uart.h"
#include "../generated/log_messages.h"
#include "../kernel/panic.h"
#include "../kernel/scheduler.h"
#include "../kernel/timer.h"
#include "../kernel/nodes.h"
#include "led_task.h"
#include "rv_demo_task.h"

#include <avr/pgmspace.h>
#include <avr/io.h>
#include <stdint.h>
#include <string.h>

#define SHELL_LINE_CAPACITY 64u
#define BINARY_GUEST_DATA_BYTES 16u
#define BINARY_GUEST_SYNC_0 0xa5u
#define BINARY_GUEST_SYNC_1 0x5au
#define BINARY_GUEST_OP_DATA 1u
#define BINARY_GUEST_OP_COMMIT 2u
#define BINARY_GUEST_OP_CANCEL 3u
#define SHELL_TEXT(s) shell_send_text_P(PSTR(s), sizeof(s) - 1u)

static const char command_help[] PROGMEM = "help";
static const char command_history[] PROGMEM = "history";
static const char command_about[] PROGMEM = "about";
static const char command_clear[] PROGMEM = "clear";
static const char command_uptime[] PROGMEM = "uptime";
static const char command_tasks[] PROGMEM = "tasks";
static const char command_stack[] PROGMEM = "stack";
static const char command_faults[] PROGMEM = "faults";
static const char command_panic[] PROGMEM = "panic";
static const char command_reboot[] PROGMEM = "reboot";
static const char command_led_on[] PROGMEM = "led on";
static const char command_led_off[] PROGMEM = "led off";
static const char command_led_blink[] PROGMEM = "led blink";
static const char command_led_status[] PROGMEM = "led status";
static const char command_guest_run[] PROGMEM = "guest run";
static const char command_guest_run_all[] PROGMEM = "guest runall";
static const char command_guest_run_all_named[] PROGMEM = "guest -runall";
static const char command_guest_list[] PROGMEM = "guest list";
static const char command_guest_directory[] PROGMEM = "guest dir";
static const char command_guest_prefix_on[] PROGMEM = "guest prefix on";
static const char command_guest_prefix_off[] PROGMEM = "guest prefix off";
static const char command_guest_prefix_status[] PROGMEM = "guest prefix status";
static const char command_guest_trace_on[] PROGMEM = "guest trace on";
static const char command_guest_trace_off[] PROGMEM = "guest trace off";
static const char command_guest_trace_status[] PROGMEM = "guest trace status";
static const char command_guest_info[] PROGMEM = "guest info";
static const char command_guest_regs[] PROGMEM = "guest regs";
static const char command_guest_builtin[] PROGMEM = "guest builtin";
static const char command_guest_binary_load_prefix[] PROGMEM = "guest bload ";
static const char command_guest_prefix[] PROGMEM = "guest ";
static const char command_restart_prefix[] PROGMEM = "restart ";

static uint16_t guest_load_expected_size;
static uint16_t guest_load_expected_crc;
static uint16_t guest_load_running_crc;
static uint16_t guest_load_received;
static uint16_t guest_load_entry_point;
static uint8_t guest_load_session;
static uint8_t guest_load_slot;
enum { AUTO_NONE, AUTO_BATCH, AUTO_REPEAT, AUTO_VERIFY };
#define RESULT_HISTORY_CAPACITY 3u
typedef struct {
    uint16_t id;
    uint32_t result;
    char name[RV_DEMO_NAME_CAPACITY];
    uint8_t state;
} shell_result_t;
static uint8_t automation_mode;
static uint8_t batch_mask, batch_total, batch_done, batch_failed;
static uint8_t repeat_left, repeat_total, repeat_done, repeat_failed;
static uint8_t verify_phase;
#if NODE_ROLE == 1
static uint32_t verify_first_result;
#endif
static char automation_name[RV_DEMO_NAME_CAPACITY];
static shell_result_t result_history[RESULT_HISTORY_CAPACITY];
static uint8_t result_history_head, result_history_count;
static void buffer_append_char(char *buffer, uint8_t *length, char value);
static void buffer_append_u32(char *buffer, uint8_t *length, uint32_t value);

static void shell_send_prompt(void)
{
#if NODE_ROLE == 1
    uart_write_string("uno$ ");
#elif NODE_ROLE == 2
    uart_write_string("nano$ ");
#else
    uart_write_string("os$ ");
#endif
}

static void shell_send_compressed(const uint8_t *data, uint16_t byte_count,
                                  uint16_t bit_count)
{
    uart_write_compressed_P(data, byte_count, bit_count);
}

static void shell_send_text(const char *text, uint8_t length)
{
    uart_write_binary_frame(UART_FRAME_SHELL_TEXT,
                            (const uint8_t *)text, length);
}

static void shell_send_text_P(const char *text, uint8_t length)
{
    uart_write_binary_frame_P(UART_FRAME_SHELL_TEXT,
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

static void buffer_append_hex16(char *buffer, uint8_t *length, uint16_t value)
{
    static const char digits[] PROGMEM = "0123456789abcdef";

    for (uint8_t shift = 12u;; shift -= 4u)
    {
        buffer_append_char(buffer, length,
                           (char)pgm_read_byte(&digits[(value >> shift) & 0x0fu]));
        if (shift == 0u)
        {
            break;
        }
    }
}

static void shell_send_uptime(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("uptime_ms="));
    buffer_append_u32(buffer, &length, timer_millis());
    shell_send_text(buffer, length);
}

static void shell_send_about(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("cpu=ATmega328P-compatible mode="));
#if LGT_SAFE_MODE
    buffer_append_P(buffer, &length, PSTR("LGT-safe"));
#else
    buffer_append_P(buffer, &length, PSTR("preemptive"));
#endif
    shell_send_text(buffer, length);

    length = 0u;
    buffer_append_P(buffer, &length, PSTR("f_cpu="));
    buffer_append_u32(buffer, &length, F_CPU);
    buffer_append_P(buffer, &length, PSTR(" tick_hz="));
    buffer_append_u32(buffer, &length, KERNEL_TICK_HZ);
    shell_send_text(buffer, length);
}

extern uint8_t _end;

static void shell_send_memory(char *buffer)
{
    uint8_t length = 0u;
    uint16_t static_end = (uint16_t)(uintptr_t)&_end;
    uint16_t stack_pointer = SP;
    uint16_t static_bytes = static_end > RAMSTART
                                ? (uint16_t)(static_end - RAMSTART) : 0u;
    uint16_t live_gap = stack_pointer > static_end
                            ? (uint16_t)(stack_pointer - static_end) : 0u;

    buffer_append_P(buffer, &length, PSTR("sram total=2048 static="));
    buffer_append_u32(buffer, &length, static_bytes);
    buffer_append_P(buffer, &length, PSTR(" live_gap="));
    buffer_append_u32(buffer, &length, live_gap);
    shell_send_text(buffer, length);

    length = 0u;
    buffer_append_P(buffer, &length, PSTR("guest_slots="));
    buffer_append_u32(buffer, &length, rv_demo_slot_count());
    buffer_append_P(buffer, &length, PSTR(" guest_storage="));
    buffer_append_u32(buffer, &length, rv_demo_storage_bytes());
    shell_send_text(buffer, length);
}

static void shell_send_led_status(char *buffer)
{
    uint8_t length = 0u;
    uint8_t mode = led_task_get_mode();

    buffer_append_P(buffer, &length, PSTR("led="));
    if (mode == LED_MODE_ON)
    {
        buffer_append_P(buffer, &length, PSTR("on"));
    }
    else if (mode == LED_MODE_OFF)
    {
        buffer_append_P(buffer, &length, PSTR("off"));
    }
    else
    {
        buffer_append_P(buffer, &length, PSTR("blink"));
#if LGT_SAFE_MODE
        buffer_append_P(buffer, &length, PSTR(" (service disabled)"));
#endif
    }
    shell_send_text(buffer, length);
}

static void shell_send_guest_info(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("guest slot=0 name="));
    {
        const char *name = rv_demo_slot_name(0u);
        while (*name != '\0')
        {
            buffer_append_char(buffer, &length, *name++);
        }
    }
    buffer_append_P(buffer, &length, PSTR(" image="));
    buffer_append_u32(buffer, &length, rv_demo_image_size());
    buffer_append_P(buffer, &length, PSTR(" memory="));
    buffer_append_u32(buffer, &length, rv_demo_memory_size());
    buffer_append_P(buffer, &length, PSTR(" active="));
    buffer_append_u32(buffer, &length, rv_demo_is_active());
    shell_send_text(buffer, length);

    length = 0u;
    buffer_append_P(buffer, &length, PSTR("source="));
    if (rv_demo_image_is_uploaded())
    {
        buffer_append_P(buffer, &length, PSTR("uploaded"));
    }
    else
    {
        buffer_append_P(buffer, &length, PSTR("builtin"));
    }
    buffer_append_char(buffer, &length, ' ');
    buffer_append_P(buffer, &length, PSTR("status="));
    buffer_append_u32(buffer, &length, rv_demo_status());
    buffer_append_P(buffer, &length, PSTR(" exit="));
    buffer_append_u32(buffer, &length, rv_demo_exit_code());
    shell_send_text(buffer, length);
}

static void shell_send_guest_regs(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("pc="));
    buffer_append_u32(buffer, &length, rv_demo_pc());
    buffer_append_P(buffer, &length, PSTR(" sp="));
    buffer_append_u32(buffer, &length, rv_demo_register(2u));
    buffer_append_P(buffer, &length, PSTR(" a0="));
    buffer_append_u32(buffer, &length, rv_demo_register(10u));
    buffer_append_P(buffer, &length, PSTR(" a7="));
    buffer_append_u32(buffer, &length, rv_demo_register(17u));
    shell_send_text(buffer, length);
}

static void shell_send_guest_list(char *buffer)
{
    for (uint8_t slot = 0u; slot < rv_demo_slot_count(); slot++)
    {
        if (rv_demo_slot_loaded(slot))
        {
            uint8_t length = 0u;

            buffer_append_P(buffer, &length, PSTR("id="));
            buffer_append_u32(buffer, &length, slot);
            buffer_append_P(buffer, &length, PSTR(" name="));
            const char *name = rv_demo_slot_name(slot);
            while (*name != '\0')
            {
                buffer_append_char(buffer, &length, *name++);
            }
            buffer_append_P(buffer, &length, PSTR(" size="));
            buffer_append_u32(buffer, &length,
                              rv_demo_slot_image_size(slot));
            buffer_append_P(buffer, &length, PSTR(" crc="));
            buffer_append_hex16(buffer, &length,
                                rv_demo_slot_image_crc(slot));
            buffer_append_P(buffer, &length, PSTR(" pri="));
            buffer_append_u32(buffer, &length, rv_demo_slot_priority(slot));
            buffer_append_P(buffer, &length, PSTR(" entry="));
            buffer_append_u32(buffer, &length,
                              rv_demo_slot_entry_point(slot));
            shell_send_text(buffer, length);
        }
    }
}

static void shell_clear_screen(void)
{
    uart_write_char('\x1b');
    uart_write_char('[');
    uart_write_char('2');
    uart_write_char('J');
    uart_write_char('\x1b');
    uart_write_char('[');
    uart_write_char('H');
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
#if LGT_SAFE_MODE
    uint8_t length = 0u;

    /* Safe mode intentionally stays on the C startup stack at RAMEND; the
     * TCB's small synthetic frame is never loaded and is not a measurement
     * of the live stack. */
    buffer_append_P(buffer, &length, PSTR("task=0 stack=main(RAMEND)"));
    shell_send_text(buffer, length);
#else
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
#endif
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

    buffer_append_P(buffer, &length, PSTR(" uart_err="));
    buffer_append_u32(buffer, &length, uart_rx_error_count());
    buffer_append_char(buffer, &length, '/');
    buffer_append_u32(buffer, &length, uart_rx_error_flags());

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

static uint8_t shell_parse_guest_name(const char **text, char *name)
{
    uint8_t length = 0u;

    while (**text != '\0' && **text != ' ' && **text != '\t')
    {
        char value = **text;
        if (length >= RV_DEMO_NAME_CAPACITY - 1u ||
            !((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-'))
        {
            return 0u;
        }
        name[length++] = value;
        (*text)++;
    }
    name[length] = '\0';
    while (**text == ' ' || **text == '\t')
    {
        (*text)++;
    }
    return length != 0u;
}

static uint16_t shell_crc16_update(uint16_t crc, uint8_t value)
{
    crc ^= (uint16_t)value << 8u;
    for (uint8_t bit = 0u; bit < 8u; bit++)
    {
        crc = (crc & 0x8000u) != 0u
                  ? (uint16_t)((crc << 1u) ^ 0x1021u)
                  : (uint16_t)(crc << 1u);
    }
    return crc;
}

static uint8_t shell_hex_nibble(char value, uint8_t *nibble)
{
    if (value >= '0' && value <= '9')
    {
        *nibble = (uint8_t)(value - '0');
        return 1u;
    }
    if (value >= 'a' && value <= 'f')
    {
        *nibble = (uint8_t)(value - 'a' + 10u);
        return 1u;
    }
    if (value >= 'A' && value <= 'F')
    {
        *nibble = (uint8_t)(value - 'A' + 10u);
        return 1u;
    }
    return 0u;
}

static void shell_cancel_guest_load(void)
{
    rv_demo_upload_cancel_slot(guest_load_slot);
    guest_load_session = 0u;
    guest_load_received = 0u;
}

static void shell_start_guest_load(char *buffer, const char *name,
                                   uint16_t image_size,
                                   uint16_t expected_crc,
                                   uint16_t entry_point)
{
    uint8_t length = 0u;
    int8_t slot;

    shell_cancel_guest_load();
    /* A PC-backed build has one resident cache slot. Loading another image
     * deliberately evicts the previous one once no VM is running. */
    slot = rv_demo_upload_begin_cached(name, image_size);
    if (slot < 0)
    {
        buffer_append_P(buffer, &length,
                        PSTR("LOAD ERROR busy-size-name-or-capacity"));
        shell_send_text(buffer, length);
        return;
    }

    guest_load_expected_size = image_size;
    guest_load_slot = (uint8_t)slot;
    guest_load_expected_crc = expected_crc;
    guest_load_entry_point = entry_point;
    guest_load_running_crc = 0xffffu;
    guest_load_received = 0u;
    guest_load_session = 1u;
    buffer_append_P(buffer, &length, PSTR("READY name="));
    for (uint8_t index = 0u; name[index] != '\0'; index++)
    {
        buffer_append_char(buffer, &length, name[index]);
    }
    buffer_append_P(buffer, &length, PSTR(" size="));
    buffer_append_u32(buffer, &length, image_size);
    buffer_append_P(buffer, &length, PSTR(" chunk=16 encoding=binary"));
    shell_send_text(buffer, length);
}

static uint8_t shell_parse_decimal(const char **text, uint16_t *value)
{
    uint16_t result = 0u;

    if (**text < '0' || **text > '9')
    {
        return 0u;
    }
    while (**text >= '0' && **text <= '9')
    {
        uint8_t digit = (uint8_t)(**text - '0');

        if (result > 19u || (result == 19u && digit > 2u))
        {
            return 0u;
        }
        result = (uint16_t)(result * 10u + digit);
        (*text)++;
    }
    *value = result;
    return 1u;
}

static uint8_t shell_skip_space(const char **text)
{
    uint8_t found = 0u;

    while (**text == ' ' || **text == '\t')
    {
        found = 1u;
        (*text)++;
    }
    return found;
}

static uint8_t shell_parse_hex16_token(const char **text, uint16_t *value)
{
    uint16_t result = 0u;

    for (uint8_t digit = 0u; digit < 4u; digit++)
    {
        uint8_t nibble;

        if (!shell_hex_nibble(**text, &nibble))
        {
            return 0u;
        }
        result = (uint16_t)((result << 4u) | nibble);
        (*text)++;
    }
    *value = result;
    return 1u;
}

static uint8_t shell_parse_binary_guest_load(const char *text, char *name,
                                             uint16_t *image_size,
                                             uint16_t *expected_crc,
                                             uint16_t *entry_point)
{
    return shell_parse_guest_name(&text, name) &&
           shell_parse_decimal(&text, image_size) &&
           *image_size != 0u && *image_size <= rv_demo_memory_size() &&
           shell_skip_space(&text) &&
           shell_parse_hex16_token(&text, expected_crc) &&
           shell_skip_space(&text) &&
           shell_parse_decimal(&text, entry_point) && *text == '\0' &&
           *entry_point < *image_size && (*entry_point & 3u) == 0u;
}

static uint8_t shell_accept_binary_data(uint16_t offset,
                                        const uint8_t *bytes,
                                        uint8_t byte_count)
{
    uint16_t end = (uint16_t)(offset + byte_count);

    if (!guest_load_session || byte_count == 0u ||
        byte_count > BINARY_GUEST_DATA_BYTES ||
        offset > guest_load_received || end < offset ||
        end > guest_load_expected_size ||
        (offset < guest_load_received && end > guest_load_received))
    {
        return 0u;
    }

    for (uint8_t index = 0u; index < byte_count; index++)
    {
        if (!rv_demo_upload_write_slot(guest_load_slot,
                                       (uint16_t)(offset + index),
                                       bytes[index]))
        {
            return 0u;
        }
    }

    if (offset == guest_load_received)
    {
        for (uint8_t index = 0u; index < byte_count; index++)
        {
            guest_load_running_crc =
                shell_crc16_update(guest_load_running_crc, bytes[index]);
        }
        guest_load_received = end;
    }
    else
    {
        /* Lost ACK: an absolute-offset packet may be resent safely. */
        guest_load_running_crc =
            rv_demo_upload_crc16_slot(guest_load_slot,
                                      guest_load_received);
    }
    return 1u;
}

static void shell_commit_guest_load(char *buffer)
{
    uint8_t length = 0u;
    uint16_t stored_crc =
        rv_demo_upload_crc16_slot(guest_load_slot, guest_load_received);

    if (!guest_load_session ||
        guest_load_received != guest_load_expected_size ||
        guest_load_running_crc != guest_load_expected_crc ||
        stored_crc != guest_load_expected_crc ||
        !rv_demo_upload_commit_metadata_slot(guest_load_slot,
                                             guest_load_expected_crc,
                                             guest_load_entry_point))
    {
        buffer_append_P(buffer, &length, PSTR("LOAD ERROR no-session"));
        if (guest_load_session)
        {
            length = 0u;
            buffer_append_P(buffer, &length, PSTR("LOAD ERROR bytes="));
            buffer_append_u32(buffer, &length, guest_load_received);
            buffer_append_char(buffer, &length, '/');
            buffer_append_u32(buffer, &length, guest_load_expected_size);
            buffer_append_P(buffer, &length, PSTR(" crc="));
            buffer_append_hex16(buffer, &length, guest_load_running_crc);
            buffer_append_char(buffer, &length, '/');
            buffer_append_hex16(buffer, &length, guest_load_expected_crc);
        }
        shell_send_text(buffer, length);
        shell_cancel_guest_load();
        return;
    }

    buffer_append_P(buffer, &length, PSTR("LOAD OK bytes="));
    buffer_append_u32(buffer, &length, guest_load_expected_size);
    buffer_append_P(buffer, &length, PSTR(" crc="));
    buffer_append_hex16(buffer, &length, guest_load_running_crc);
    shell_send_text(buffer, length);
    guest_load_session = 0u;
}

static uint8_t shell_binary_read_byte(uint8_t *value)
{
    char input;

    if (uart_read_char_timeout(&input, 5000u) != WAIT_ACQUIRED)
    {
        return 0u;
    }
    *value = (uint8_t)input;
    return 1u;
}

static void shell_send_binary_ack(char *buffer)
{
    uint8_t length = 0u;

    buffer_append_P(buffer, &length, PSTR("BINARY ACK bytes="));
    buffer_append_u32(buffer, &length, guest_load_received);
    buffer_append_P(buffer, &length, PSTR(" crc="));
    buffer_append_hex16(buffer, &length,
                        rv_demo_upload_crc16_slot(guest_load_slot,
                                                  guest_load_received));
    shell_send_text(buffer, length);
}

static void shell_receive_binary_guest(char *buffer)
{
    uint8_t payload[BINARY_GUEST_DATA_BYTES];

    while (guest_load_session)
    {
        uint8_t value;
        uint8_t previous = 0u;
        uint8_t operation;
        uint8_t offset_low;
        uint8_t offset_high;
        uint8_t byte_count;
        uint8_t crc_low;
        uint8_t crc_high;
        uint16_t packet_crc = 0xffffu;
        uint16_t received_crc;
        uint16_t offset;

        /* Search for A5 5A so a damaged/truncated packet can be abandoned at
         * the next retry without resetting the shell. */
        do
        {
            if (!shell_binary_read_byte(&value))
            {
                shell_cancel_guest_load();
                shell_send_text_P(PSTR("BINARY TIMEOUT"), 14u);
                return;
            }
            if (previous == BINARY_GUEST_SYNC_0 &&
                value == BINARY_GUEST_SYNC_1)
            {
                break;
            }
            previous = value;
        } while (1);

        if (!shell_binary_read_byte(&operation) ||
            !shell_binary_read_byte(&offset_low) ||
            !shell_binary_read_byte(&offset_high) ||
            !shell_binary_read_byte(&byte_count) ||
            byte_count > BINARY_GUEST_DATA_BYTES)
        {
            shell_send_text_P(PSTR("BINARY NACK header"), 18u);
            continue;
        }

        packet_crc = shell_crc16_update(packet_crc, operation);
        packet_crc = shell_crc16_update(packet_crc, offset_low);
        packet_crc = shell_crc16_update(packet_crc, offset_high);
        packet_crc = shell_crc16_update(packet_crc, byte_count);
        for (uint8_t index = 0u; index < byte_count; index++)
        {
            if (!shell_binary_read_byte(&payload[index]))
            {
                shell_cancel_guest_load();
                shell_send_text_P(PSTR("BINARY TIMEOUT"), 14u);
                return;
            }
            packet_crc = shell_crc16_update(packet_crc, payload[index]);
        }
        if (!shell_binary_read_byte(&crc_low) ||
            !shell_binary_read_byte(&crc_high))
        {
            shell_cancel_guest_load();
            shell_send_text_P(PSTR("BINARY TIMEOUT"), 14u);
            return;
        }

        received_crc = (uint16_t)crc_low | ((uint16_t)crc_high << 8u);
        offset = (uint16_t)offset_low | ((uint16_t)offset_high << 8u);
        if (received_crc != packet_crc)
        {
            shell_send_text_P(PSTR("BINARY NACK crc"), 15u);
            continue;
        }

        if (operation == BINARY_GUEST_OP_DATA &&
            shell_accept_binary_data(offset, payload, byte_count))
        {
            shell_send_binary_ack(buffer);
        }
        else if (operation == BINARY_GUEST_OP_COMMIT && byte_count == 0u)
        {
            shell_commit_guest_load(buffer);
            return;
        }
        else if (operation == BINARY_GUEST_OP_CANCEL && byte_count == 0u)
        {
            shell_cancel_guest_load();
            shell_send_text_P(PSTR("BINARY CANCELLED"), 16u);
            return;
        }
        else
        {
            shell_send_text_P(PSTR("BINARY NACK state"), 17u);
        }
    }
}

static void shell_show_job(char *line, job_info_t job)
{
    if (!job.id) { SHELL_TEXT("[INFO] Nothing has been run yet. Try: run builtin"); return; }
    uint8_t n = 0;
    buffer_append_char(line, &n, '[');
    buffer_append_P(line, &n, job.state == JOB_COPYING ? PSTR("COPYING") :
                        job.state == JOB_DONE ? PSTR("DONE") :
                        job.state == JOB_RUNNING ? PSTR("RUNNING") :
                        job.state == JOB_QUEUED ? PSTR("WAIT") :
                        job.state == JOB_CANCELLED ? PSTR("STOPPED") :
                        job.state == JOB_REJECTED ? PSTR("NOT-RUN") :
                        job.state == JOB_FAULT ? PSTR("FAILED") : PSTR("UNKNOWN"));
    buffer_append_P(line, &n, PSTR(" #"));
    buffer_append_u32(line, &n, job.id);
    buffer_append_P(line, &n, PSTR("] "));
    const char *name = nodes_job_name();
    for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
    if (job.state == JOB_COPYING) {
        uint16_t size = 0;
        for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++)
            if (rv_demo_slot_loaded(slot) && !strcmp(rv_demo_slot_name(slot), name))
                size = rv_demo_slot_image_size(slot);
        buffer_append_char(line, &n, ' ');
        buffer_append_u32(line, &n, job.result);
        buffer_append_char(line, &n, '/');
        buffer_append_u32(line, &n, size);
        buffer_append_P(line, &n, PSTR(" bytes Uno -> Nano."));
    } else buffer_append_P(line, &n, job.state == JOB_DONE ? PSTR(" returned ") :
                        job.state == JOB_RUNNING ? PSTR(" is running on Nano. Use kill.") :
                        job.state == JOB_QUEUED ? PSTR(" is being sent to Nano.") :
                        job.state == JOB_CANCELLED ? PSTR(" was stopped.") :
                        job.state == JOB_REJECTED ? PSTR(" was rejected by Nano.") :
                        job.state == JOB_FAULT ? PSTR(" crashed in the VM.") :
                        PSTR(" may or may not have finished."));
    if (job.state == JOB_DONE) buffer_append_u32(line, &n, job.result);
    shell_send_text(line, n);
    if (job.state == JOB_UNKNOWN) SHELL_TEXT("Use status first. Then use retry or forget.");
    else if (job.state == JOB_REJECTED) {
        if (job.detail == 1) SHELL_TEXT("It is not loaded on Nano. Type upload for instructions.");
        else if (job.detail == 2) SHELL_TEXT("Nano has a different program version. Upload it again.");
        else SHELL_TEXT("Nano is busy. Wait, then run it again.");
    }
}

static uint8_t shell_start_next_batch(void)
{
    while (batch_mask) {
        uint8_t slot = 0;
        while (!(batch_mask & (uint8_t)(1u << slot))) slot++;
        batch_mask &= (uint8_t)~(1u << slot);
        if (rv_demo_slot_loaded(slot) &&
            nodes_deploy_run(rv_demo_slot_name(slot), TASK_PRIORITY_NORMAL))
            return 1;
        batch_failed++;
    }
    return 0;
}

#if NODE_ROLE == 1
static void shell_record_result(job_info_t job)
{
    if (!job.id || job.state == JOB_UNKNOWN) return;
    shell_result_t *record = &result_history[result_history_head];
    record->id = job.id;
    record->result = job.result;
    record->state = job.state;
    memset(record->name, 0, sizeof(record->name));
    strncpy(record->name, nodes_job_name(), sizeof(record->name) - 1u);
    result_history_head = (uint8_t)((result_history_head + 1u) % RESULT_HISTORY_CAPACITY);
    if (result_history_count < RESULT_HISTORY_CAPACITY) result_history_count++;
}
#endif

static void shell_show_results(char *line)
{
    if (!result_history_count) { SHELL_TEXT("No completed results yet."); return; }
    SHELL_TEXT("Newest completed results:");
    for (uint8_t age = 0; age < result_history_count; age++) {
        uint8_t index = (uint8_t)((result_history_head + RESULT_HISTORY_CAPACITY - 1u - age) %
                                  RESULT_HISTORY_CAPACITY);
        const shell_result_t *record = &result_history[index];
        uint8_t n = 0;
        buffer_append_char(line, &n, '#');
        buffer_append_u32(line, &n, record->id);
        buffer_append_char(line, &n, ' ');
        for (uint8_t i = 0; record->name[i]; i++) buffer_append_char(line, &n, record->name[i]);
        buffer_append_P(line, &n, record->state == JOB_DONE ? PSTR(" DONE=") :
                                  record->state == JOB_CANCELLED ? PSTR(" STOPPED") :
                                  record->state == JOB_FAULT ? PSTR(" FAILED") :
                                  record->state == JOB_REJECTED ? PSTR(" REJECTED") :
                                  PSTR(" ENDED"));
        if (record->state == JOB_DONE) buffer_append_u32(line, &n, record->result);
        shell_send_text(line, n);
    }
}

#if NODE_ROLE == 1
static void shell_finish_automation(char *line, job_info_t job)
{
    if (automation_mode == AUTO_BATCH) {
        if (job.state == JOB_DONE) batch_done++;
        else batch_failed++;
        if (job.state == JOB_UNKNOWN) batch_mask = 0;
        if (batch_mask && shell_start_next_batch()) {
            uint8_t n = 0;
            buffer_append_P(line, &n, PSTR("[NEXT] Starting "));
            const char *name = nodes_job_name();
            for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
            buffer_append_char(line, &n, '.');
            shell_send_text(line, n);
            return;
        }
        uint8_t n = 0;
        buffer_append_P(line, &n, job.state == JOB_UNKNOWN ? PSTR("[BATCH PAUSED] passed=") :
                                                            PSTR("[BATCH DONE] passed="));
        buffer_append_u32(line, &n, batch_done);
        buffer_append_P(line, &n, PSTR(" failed="));
        buffer_append_u32(line, &n, batch_failed);
        shell_send_text(line, n);
        automation_mode = AUTO_NONE;
        batch_total = 0;
        return;
    }

    if (automation_mode == AUTO_REPEAT) {
        if (job.state == JOB_DONE) repeat_done++;
        else repeat_failed++;
        if (job.state != JOB_UNKNOWN && repeat_left &&
            nodes_deploy_run(automation_name, TASK_PRIORITY_NORMAL)) {
            repeat_left--;
            uint8_t n = 0;
            buffer_append_P(line, &n, PSTR("[REPEAT] Starting run "));
            buffer_append_u32(line, &n, (uint8_t)(repeat_total - repeat_left));
            buffer_append_char(line, &n, '/');
            buffer_append_u32(line, &n, repeat_total);
            shell_send_text(line, n);
            return;
        }
        uint8_t n = 0;
        buffer_append_P(line, &n, job.state == JOB_UNKNOWN ? PSTR("[REPEAT PAUSED] passed=") :
                                                            PSTR("[REPEAT DONE] passed="));
        buffer_append_u32(line, &n, repeat_done);
        buffer_append_P(line, &n, PSTR(" failed="));
        buffer_append_u32(line, &n, repeat_failed);
        shell_send_text(line, n);
        automation_mode = AUTO_NONE;
        return;
    }

    if (automation_mode == AUTO_VERIFY) {
        if (verify_phase == 1u && job.state == JOB_DONE &&
            nodes_deploy_run(automation_name, TASK_PRIORITY_NORMAL)) {
            verify_first_result = job.result;
            verify_phase = 2u;
            SHELL_TEXT("[VERIFY] First run finished. Starting comparison run.");
            return;
        }
        if (verify_phase == 2u && job.state == JOB_DONE && job.result == verify_first_result) {
            uint8_t n = 0;
            buffer_append_P(line, &n, PSTR("[VERIFY PASS] Both runs returned "));
            buffer_append_u32(line, &n, job.result);
            shell_send_text(line, n);
        } else if (verify_phase == 2u && job.state == JOB_DONE) {
            uint8_t n = 0;
            buffer_append_P(line, &n, PSTR("[VERIFY FAIL] first="));
            buffer_append_u32(line, &n, verify_first_result);
            buffer_append_P(line, &n, PSTR(" second="));
            buffer_append_u32(line, &n, job.result);
            shell_send_text(line, n);
        } else {
            SHELL_TEXT("[VERIFY FAILED] A run did not finish normally.");
        }
        automation_mode = AUTO_NONE;
        verify_phase = 0;
    }
}
#endif

static void shell_show_job_debug(char *line)
{
    job_info_t job = nodes_job();
    uint8_t n = 0;
    buffer_append_P(line, &n, PSTR("id=")); buffer_append_u32(line, &n, job.id);
    buffer_append_P(line, &n, PSTR(" state=")); buffer_append_u32(line, &n, job.state);
    buffer_append_P(line, &n, PSTR(" result=")); buffer_append_u32(line, &n, job.result);
    buffer_append_P(line, &n, PSTR(" detail=")); buffer_append_u32(line, &n, job.detail);
#if NODE_ROLE == 2
    buffer_append_P(line, &n, PSTR(" runs=")); buffer_append_u32(line, &n, nodes_run_count());
#endif
    shell_send_text(line, n);
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

    if (NODE_ROLE == 0 && (strcmp_P(line, PSTR("ls")) == 0 ||
                           strcmp_P(line, PSTR("list")) == 0 ||
                           strcmp_P(line, PSTR("library")) == 0))
    {
        /* The Python monitor recognizes this control line and prints the PC
         * build directory. A plain terminal still reveals what is missing. */
        SHELL_TEXT("HOST LIST");
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("ls")) == 0 ||
                           strcmp_P(line, PSTR("list")) == 0))
    {
        SHELL_TEXT("PROGRAM       LOCATION");
        uint8_t loaded = 0;
        for (uint8_t local = 0; local < rv_demo_slot_count(); local++) {
            if (!rv_demo_slot_loaded(local)) continue;
            loaded++;
            const char *name = rv_demo_slot_name(local);
            uint16_t size = rv_demo_slot_image_size(local);
            uint16_t crc = rv_demo_slot_image_crc(local);
            uint8_t on_nano = 0;
            for (uint8_t remote_slot = 0; remote_slot < nodes_program_count(); remote_slot++) {
                const node_program_t *p = nodes_program(remote_slot);
                if (p && p->seen && p->loaded && !strcmp(p->name, name) &&
                    p->size == size && p->crc == crc) on_nano = 1;
            }
            uint8_t n = 0;
            for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
            buffer_append_P(line, &n, on_nano ? PSTR("  uno+nano") : PSTR("  uno"));
            shell_send_text(line, n);
        }
        if (!loaded) {
            SHELL_TEXT("No programs. Run: load");
        } else {
            SHELL_TEXT("Use: run PROGRAM");
        }
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("programs")) == 0 ||
                          strcmp_P(line, PSTR("programs remote")) == 0))
    {
        if (nodes_info().state != 2) {
            SHELL_TEXT("Nano offline: directory unavailable. Check nodes.");
        } else {
            SHELL_TEXT("Programs ready on Nano:");
            uint8_t count = nodes_program_count(), pending = !count, loaded = 0;
            for (uint8_t slot = 0; slot < count; slot++) {
                const node_program_t *p = nodes_program(slot);
                if (!p->seen) { pending = 1; continue; }
                if (!p->loaded) continue;
                loaded++;
                uint8_t n = 0;
                for (uint8_t i = 0; p->name[i]; i++) buffer_append_char(line, &n, p->name[i]);
                buffer_append_P(line, &n, PSTR("  ["));
                buffer_append_P(line, &n, p->priority == 3 ? PSTR("high") :
                                    p->priority == 2 ? PSTR("normal") :
                                    p->priority == 1 ? PSTR("low") : PSTR("idle"));
                buffer_append_P(line, &n, PSTR("]"));
                shell_send_text(line, n);
            }
            if (pending) SHELL_TEXT("[WAIT] Still syncing. Type list again in 2 seconds.");
            else if (!loaded) SHELL_TEXT("No programs loaded on Nano.");
            else SHELL_TEXT("Start one with: run <name>");
        }
    }
    else if (strcmp_P(line, PSTR("programs local")) == 0 ||
             strcmp_P(line, PSTR("list local")) == 0 ||
             (NODE_ROLE != 1 && (strcmp_P(line, PSTR("programs")) == 0 ||
                                 strcmp_P(line, PSTR("list")) == 0 ||
                                 strcmp_P(line, PSTR("ls")) == 0)))
    {
        if (strcmp_P(line, PSTR("list")) == 0 || strcmp_P(line, PSTR("ls")) == 0) {
            SHELL_TEXT("Programs on this board:");
            uint8_t loaded = 0;
            for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++) {
                if (!rv_demo_slot_loaded(slot)) continue;
                loaded++;
                uint8_t n = 0;
                const char *name = rv_demo_slot_name(slot);
                for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
                buffer_append_P(line, &n, PSTR("  ["));
                uint8_t priority = rv_demo_slot_priority(slot);
                buffer_append_P(line, &n, priority == 3 ? PSTR("high") :
                                    priority == 2 ? PSTR("normal") :
                                    priority == 1 ? PSTR("low") : PSTR("idle"));
                buffer_append_P(line, &n, PSTR("]"));
                shell_send_text(line, n);
            }
            if (!loaded) SHELL_TEXT("No programs loaded.");
            else SHELL_TEXT("Start one with: run <name>");
        } else {
            SHELL_TEXT("Programs on THIS board (diagnostic view):");
            shell_send_guest_list(line);
        }
    }
    else if (NODE_ROLE == 1 && strcmp_P(line, PSTR("job resend")) == 0)
    {
        if (nodes_resend())
            shell_send_text_P(PSTR("resending same job ID"), sizeof("resending same job ID")-1u);
        else shell_send_text_P(PSTR("no safe job to resend"), sizeof("no safe job to resend")-1u);
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("kill")) == 0 ||
                               strcmp_P(line, PSTR("stop")) == 0 ||
                               strcmp_P(line, PSTR("job cancel")) == 0))
    {
        batch_mask = 0; /* stop means stop the whole user-requested batch */
        repeat_left = 0;
        if (nodes_cancel()) SHELL_TEXT("[WAIT] Asking Nano to stop. Waiting for confirmation.");
        else SHELL_TEXT("[INFO] Nothing is running, or Nano is offline.");
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("forget")) == 0 ||
                               strcmp_P(line, PSTR("job discard")) == 0))
    {
        if (nodes_discard()) SHELL_TEXT("Uncertain record discarded. You may submit a new job.");
        else SHELL_TEXT("Discard is only for an UNKNOWN job.");
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("retry yes")) == 0 ||
                               strcmp_P(line, PSTR("job retry confirm")) == 0))
    {
        if (nodes_retry()) SHELL_TEXT("Submitted a NEW job ID. The old job may have run too.");
        else SHELL_TEXT("Retry requires ONLINE Nano and an UNKNOWN non-cancel job.");
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("retry")) == 0 ||
                               strcmp_P(line, PSTR("job retry")) == 0))
    {
        SHELL_TEXT("[WARNING] The old run may already have changed something.");
        SHELL_TEXT("For pure calculations only, confirm with: retry yes");
    }
    else if (NODE_ROLE == 1 && strcmp_P(line, PSTR("results")) == 0)
    {
        shell_show_results(line);
    }
    else if (NODE_ROLE == 1 && strcmp_P(line, PSTR("results clear")) == 0)
    {
        result_history_head = result_history_count = 0;
        SHELL_TEXT("Completed-result history cleared.");
    }
    else if (NODE_ROLE == 1 && (strncmp_P(line, PSTR("run -n "), 7u) == 0 ||
                               strncmp_P(line, PSTR("repeat "), 7u) == 0))
    {
        const char *args = line + 7u;
        char name[RV_DEMO_NAME_CAPACITY];
        uint16_t count = 0;
        uint8_t unix_form = line[0] == 'r' && line[1] == 'u';
        uint8_t valid = unix_form ?
                        (shell_parse_decimal(&args, &count) && shell_skip_space(&args) &&
                         shell_parse_guest_name(&args, name) && !*args) :
                        (shell_parse_guest_name(&args, name) &&
                         shell_parse_decimal(&args, &count) && !*args);
        valid = valid && count && count <= 99u;
        if (!valid) {
            SHELL_TEXT("usage: run -n COUNT PROGRAM   (COUNT=1..99)");
        } else if (automation_mode != AUTO_NONE) {
            SHELL_TEXT("[BUSY] An automatic test is already running. Use kill.");
        } else if (nodes_info().state != 2) {
            SHELL_TEXT("[OFFLINE] Nano cannot be reached. Type status.");
        } else if (!nodes_deploy_run(name, TASK_PRIORITY_NORMAL)) {
            SHELL_TEXT("[NOT STARTED] Check the name with list, or wait for the current run.");
        } else {
            memset(automation_name, 0, sizeof(automation_name));
            strncpy(automation_name, name, sizeof(automation_name) - 1u);
            automation_mode = AUTO_REPEAT;
            repeat_total = (uint8_t)count;
            repeat_left = (uint8_t)(count - 1u);
            repeat_done = repeat_failed = 0;
            uint8_t n = 0;
            buffer_append_P(line, &n, PSTR("[REPEAT] Running "));
            for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
            buffer_append_char(line, &n, ' ');
            buffer_append_u32(line, &n, count);
            buffer_append_P(line, &n, PSTR(" times. Use kill to cancel."));
            shell_send_text(line, n);
        }
    }
    else if (NODE_ROLE == 1 && (strncmp_P(line, PSTR("run --verify "), 13u) == 0 ||
                               strncmp_P(line, PSTR("verify "), 7u) == 0))
    {
        const char *args = line + (line[4] == '-' ? 13u : 7u);
        char name[RV_DEMO_NAME_CAPACITY];
        if (!shell_parse_guest_name(&args, name) || *args) {
            SHELL_TEXT("usage: run --verify PROGRAM");
        } else if (automation_mode != AUTO_NONE) {
            SHELL_TEXT("[BUSY] An automatic test is already running. Use kill.");
        } else if (nodes_info().state != 2) {
            SHELL_TEXT("[OFFLINE] Nano cannot be reached. Type status.");
        } else if (!nodes_deploy_run(name, TASK_PRIORITY_NORMAL)) {
            SHELL_TEXT("[NOT STARTED] Check the name with list, or wait for the current run.");
        } else {
            memset(automation_name, 0, sizeof(automation_name));
            strncpy(automation_name, name, sizeof(automation_name) - 1u);
            automation_mode = AUTO_VERIFY;
            verify_phase = 1u;
            SHELL_TEXT("[VERIFY] Running the same image twice and comparing exit results.");
            SHELL_TEXT("Use kill to cancel.");
        }
    }
    else if (NODE_ROLE == 1 && (strcmp_P(line, PSTR("run --all")) == 0 ||
                               strcmp_P(line, PSTR("run all")) == 0))
    {
        if (automation_mode != AUTO_NONE) {
            SHELL_TEXT("[BUSY] An automatic test is already running. Use kill.");
        } else if (nodes_info().state != 2) {
            SHELL_TEXT("[OFFLINE] Nano cannot be reached. Type status.");
        } else {
            batch_mask = batch_total = batch_done = batch_failed = 0;
            for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++)
                if (rv_demo_slot_loaded(slot)) {
                    batch_mask |= (uint8_t)(1u << slot);
                    batch_total++;
                }
            if (!batch_total) SHELL_TEXT("No programs on Uno. Type upload.");
            else if (shell_start_next_batch()) {
                automation_mode = AUTO_BATCH;
                uint8_t n = 0;
                buffer_append_P(line, &n, PSTR("[BATCH] Running "));
                buffer_append_u32(line, &n, batch_total);
                buffer_append_P(line, &n, PSTR(" programs. Use kill to cancel."));
                shell_send_text(line, n);
            } else {
                SHELL_TEXT("[BATCH FAILED] No program could be started.");
                batch_mask = batch_total = 0;
            }
        }
    }
    else if (NODE_ROLE == 1 && (strncmp_P(line, PSTR("run "), 4u) == 0 ||
                               strncmp_P(line, PSTR("job run "), 8u) == 0))
    {
        const char *args = line + (line[0] == 'r' ? 4u : 8u);
        char name[12];
        uint16_t priority = 2, crc = 0;
        uint8_t valid = shell_parse_guest_name(&args, name), pinned = 0;
        if (valid && *args) {
            if (strcmp_P(args, PSTR("low")) == 0) priority = 1;
            else if (strcmp_P(args, PSTR("normal")) == 0) priority = 2;
            else if (strcmp_P(args, PSTR("high")) == 0) priority = 3;
            else {
                valid = shell_parse_decimal(&args, &priority) && priority <= 3;
                if (valid && *args) {
                    valid = shell_skip_space(&args) && shell_parse_hex16_token(&args, &crc)
                            && !*args;
                    pinned = valid;
                }
            }
        }
        if (!valid) {
            SHELL_TEXT("Use: run <name> [low|normal|high]");
            SHELL_TEXT("Example: run fibonacci");
        } else if (nodes_info().state != 2) {
            SHELL_TEXT("[OFFLINE] Nano cannot be reached. Type status.");
        } else if (!(pinned ? nodes_submit(name, (uint8_t)priority, crc) :
                             nodes_deploy_run(name, (uint8_t)priority))) {
            SHELL_TEXT("[NOT STARTED] Program is not on Uno, or another run is busy.");
            SHELL_TEXT("Type list. To add programs, type upload.");
        } else {
            uint8_t n = 0;
            job_info_t started = nodes_job();
            buffer_append_char(line, &n, '[');
            buffer_append_u32(line, &n, started.id);
            buffer_append_P(line, &n, started.state == JOB_COPYING ?
                            PSTR("] copying ") : PSTR("] started "));
            for (uint8_t i = 0; name[i]; i++) buffer_append_char(line, &n, name[i]);
            buffer_append_P(line, &n, started.state == JOB_COPYING ?
                            PSTR(" Uno -> Nano. It will start automatically.") :
                            PSTR(" is running on Nano."));
            shell_send_text(line, n);
            SHELL_TEXT("Shell remains interactive. Use kill to stop the job.");
        }
    }
    else if (NODE_ROLE == 0 && strncmp_P(line, PSTR("run "), 4u) == 0)
    {
        const char *args = line + 4u;
        char name[RV_DEMO_NAME_CAPACITY];
        uint16_t priority = TASK_PRIORITY_NORMAL;
        uint8_t valid = shell_parse_guest_name(&args, name);

        if (valid && *args)
        {
            if (strcmp_P(args, PSTR("low")) == 0) priority = TASK_PRIORITY_LOW;
            else if (strcmp_P(args, PSTR("normal")) == 0) priority = TASK_PRIORITY_NORMAL;
            else if (strcmp_P(args, PSTR("high")) == 0) priority = TASK_PRIORITY_HIGH;
            else valid = 0u;
        }
        if (!valid)
        {
            SHELL_TEXT("Use: run <name> [low|normal|high]");
        }
        else
        {
            uint8_t n = 0u;
            buffer_append_P(line, &n, PSTR("HOST RUN "));
            for (uint8_t i = 0u; name[i] != '\0'; i++)
                buffer_append_char(line, &n, name[i]);
            buffer_append_char(line, &n, ' ');
            buffer_append_u32(line, &n, priority);
            shell_send_text(line, n);
        }
    }
    else if (NODE_ROLE == 0 && strncmp_P(line, PSTR("run-cached "), 11u) == 0)
    {
        const char *args = line + 11u;
        char name[RV_DEMO_NAME_CAPACITY];
        uint16_t priority = TASK_PRIORITY_NORMAL;
        uint8_t valid = shell_parse_guest_name(&args, name);

        if (valid && *args)
            valid = shell_parse_decimal(&args, &priority) && priority <= 3u && !*args;
        if (!valid || rv_demo_find_name(name) < 0)
        {
            SHELL_TEXT("[HOST ERROR] demanded image was not loaded");
        }
        else
        {
            (void)rv_demo_set_priority(name, (uint8_t)priority);
            SHELL_TEXT("[RUNNING] Uno VM started. Press q to stop it.");
            rv_demo_set_output_prefix(1u);
            if (!rv_demo_run_name(name)) SHELL_TEXT("[FAILED] VM could not start.");
            else if (rv_demo_was_cancelled()) SHELL_TEXT("[STOPPED] Program was interrupted.");
            else SHELL_TEXT("[DONE] Program finished.");
        }
    }
    else if (NODE_ROLE == 2 && strncmp_P(line, PSTR("run "), 4u) == 0)
    {
        const char *args = line + 4u;
        char name[RV_DEMO_NAME_CAPACITY];
        if (!shell_parse_guest_name(&args, name) || *args) {
            SHELL_TEXT("Use: run <name>");
        } else {
            SHELL_TEXT("[RUNNING] Local VM started. Press q to stop it.");
            uint8_t old_prefix = rv_demo_output_prefix_enabled();
            rv_demo_set_output_prefix(1u);
            if (!rv_demo_run_name(name)) SHELL_TEXT("[NOT FOUND] Type list to see local programs.");
            else if (rv_demo_was_cancelled()) SHELL_TEXT("[STOPPED] Local program was interrupted.");
            else SHELL_TEXT("[DONE] Local program finished.");
            rv_demo_set_output_prefix(old_prefix);
        }
    }
    else if (NODE_ROLE && strcmp_P(line, PSTR("job debug")) == 0)
    {
        shell_show_job_debug(line);
    }
    else if (NODE_ROLE == 1 && strcmp_P(line, PSTR("jobs")) == 0)
    {
        job_info_t job = nodes_job();
        if (job.state == JOB_QUEUED || job.state == JOB_RUNNING ||
            job.state == JOB_COPYING || job.state == JOB_UNKNOWN)
            shell_show_job(line, job);
        else SHELL_TEXT("No active jobs.");
        shell_show_results(line);
    }
    else if (NODE_ROLE && (strcmp_P(line, PSTR("last")) == 0 ||
                          strcmp_P(line, PSTR("job status")) == 0 ||
                          strcmp_P(line, PSTR("job")) == 0))
    {
        shell_show_job(line, nodes_job());
    }
    else if (NODE_ROLE == 2 && strcmp_P(line, PSTR("nodes pause")) == 0)
    {
        nodes_pause();
        shell_send_text_P(PSTR("worker heartbeat paused"), 23u);
    }
    else if (NODE_ROLE == 2 && strcmp_P(line, PSTR("nodes resume")) == 0)
    {
        nodes_resume();
        shell_send_text_P(PSTR("worker heartbeat resumed"), 24u);
    }
    else if (strcmp_P(line, PSTR("cluster")) == 0 ||
             strcmp_P(line, PSTR("status")) == 0)
    {
        node_info_t info = nodes_info();
#if NODE_ROLE == 1
        if (info.state == 2) SHELL_TEXT("[ONLINE] Uno can reach Nano over I2C.");
        else if (info.state == 1) SHELL_TEXT("[UNSTABLE] Nano missed a reply. Check again shortly.");
        else SHELL_TEXT("[OFFLINE] Check Nano power, shared GND, A4/SDA and A5/SCL.");
#elif NODE_ROLE == 2
        (void)info;
        SHELL_TEXT("[READY] This is the Nano worker at I2C address 0x12.");
#else
        (void)info;
        SHELL_TEXT("[LOCAL] I2C distributed mode is disabled in this firmware.");
#endif
    }
    else if (strcmp_P(line, PSTR("nodes")) == 0)
    {
        node_info_t info = nodes_info();
        uint8_t n = 0;
        buffer_append_P(line, &n, NODE_ROLE == 1 ? PSTR("remote=0x12 state=") :
                        NODE_ROLE == 2 ? PSTR("local=0x12 state=") : PSTR("nodes disabled state="));
        buffer_append_P(line, &n, info.state == 2 ? PSTR("ONLINE") :
                        info.state == 1 ? PSTR("SUSPECT") : PSTR("OFFLINE"));
        buffer_append_P(line, &n, PSTR(" hb="));
        buffer_append_u32(line, &n, info.heartbeat);
        buffer_append_P(line, &n, PSTR(" failures="));
        buffer_append_u32(line, &n, info.failures);
        buffer_append_P(line, &n, PSTR(" err="));
        buffer_append_u32(line, &n, info.status);
        shell_send_text(line, n);
    }
    else if (strcmp_P(line, PSTR("advanced")) == 0)
    {
#if NODE_ROLE == 1
        shell_send_compressed(log_shell_advanced_data,
                              LOG_SHELL_ADVANCED_BYTE_COUNT,
                              LOG_SHELL_ADVANCED_BIT_COUNT);
#elif NODE_ROLE == 2
        SHELL_TEXT("KERNEL: ps | free | faults | uptime");
        SHELL_TEXT("VM: programs local | guest info | guest regs");
        SHELL_TEXT("I2C test: nodes | nodes pause | nodes resume");
#else
        SHELL_TEXT("KERNEL: ps | free | faults | uptime");
#endif
    }
    else if (strcmp_P(line, PSTR("man")) == 0)
    {
        shell_send_compressed(log_shell_man_index_data,
                              LOG_SHELL_MAN_INDEX_BYTE_COUNT,
                              LOG_SHELL_MAN_INDEX_BIT_COUNT);
    }
    else if (strncmp_P(line, PSTR("man "), 4u) == 0)
    {
        const char *topic = line + 4u;
        if (strcmp_P(topic, PSTR("ls")) == 0) {
            SHELL_TEXT("ls: list programs and cache location");
        } else if (strcmp_P(topic, PSTR("run")) == 0) {
            shell_send_compressed(log_shell_man_run_data,
                                  LOG_SHELL_MAN_RUN_BYTE_COUNT,
                                  LOG_SHELL_MAN_RUN_BIT_COUNT);
        } else if (strcmp_P(topic, PSTR("jobs")) == 0) {
            SHELL_TEXT("jobs: active job and three newest results");
        } else if (strcmp_P(topic, PSTR("kill")) == 0) {
            SHELL_TEXT("kill: cancel the active and pending runs");
        } else if (strcmp_P(topic, PSTR("cluster")) == 0) {
            SHELL_TEXT("cluster: show the Nano I2C link");
        } else if (strcmp_P(topic, PSTR("load")) == 0) {
            SHELL_TEXT("load: print PC guest-upload commands");
        } else if (strcmp_P(topic, PSTR("advanced")) == 0) {
            SHELL_TEXT("advanced: kernel and protocol tools");
        } else {
            SHELL_TEXT("No manual entry. Run: man");
        }
    }
    else if (strcmp_P(line, command_help) == 0)
    {
#if NODE_ROLE == 1
        shell_send_compressed(log_shell_help_data,
                              LOG_SHELL_HELP_BYTE_COUNT,
                              LOG_SHELL_HELP_BIT_COUNT);
#elif NODE_ROLE == 2
        shell_send_compressed(log_shell_help_worker_data,
                              LOG_SHELL_HELP_WORKER_BYTE_COUNT,
                              LOG_SHELL_HELP_WORKER_BIT_COUNT);
#else
        SHELL_TEXT("ATmega PC-backed shell");
        SHELL_TEXT("ls | run PROGRAM [low|normal|high] | history | clear");
        SHELL_TEXT("Images stay on the PC and load into Uno SRAM on demand.");
#endif
    }
    else if (strcmp_P(line, PSTR("load")) == 0 ||
             strcmp_P(line, PSTR("upload")) == 0)
    {
#if NODE_ROLE == 1
        SHELL_TEXT("Run on the PC, not inside uno$:");
        SHELL_TEXT("make guest-uno GUEST_C=path/to/file.c");
        SHELL_TEXT("make guests-uno GUEST_CS='guest-riscv/workloads/*.c'");
        SHELL_TEXT("Then use: ls; run PROGRAM");
#else
        SHELL_TEXT("Uploads come from the PC terminal over this board's COM port.");
#endif
    }
    else if (strcmp_P(line, command_history) == 0)
    {
        static const uint8_t host_history_request[] PROGMEM = {0x1f, 'H'};
        uart_write_binary_frame_P(UART_FRAME_SHELL_TEXT,
                                  host_history_request,
                                  sizeof(host_history_request));
    }
    else if (strcmp_P(line, PSTR("uname")) == 0 ||
             strcmp_P(line, command_about) == 0)
    {
        shell_send_about(line);
    }
    else if (strncmp_P(line, PSTR("echo "), 5u) == 0)
    {
        shell_send_text(line + 5u, (uint8_t)strlen(line + 5u));
    }
    else if (strcmp_P(line, PSTR("echo")) == 0)
    {
        SHELL_TEXT("");
    }
    else if (strcmp_P(line, command_clear) == 0)
    {
        shell_clear_screen();
    }
    else if (strcmp_P(line, command_uptime) == 0)
    {
        shell_send_uptime(line);
    }
    else if (strcmp_P(line, PSTR("mem")) == 0)
    {
        shell_send_memory(line);
    }
    else if (strcmp_P(line, PSTR("ps")) == 0 ||
             strcmp_P(line, command_tasks) == 0)
    {
        shell_send_tasks(line);
    }
    else if (strcmp_P(line, PSTR("free")) == 0 ||
             strcmp_P(line, command_stack) == 0)
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
    else if (strcmp_P(line, command_reboot) == 0)
    {
        kernel_reboot();
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
    else if (strcmp_P(line, command_led_status) == 0)
    {
        shell_send_led_status(line);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_run) == 0)
    {
        if (!rv_demo_run())
        {
            uint8_t length = 0u;

            buffer_append_P(line, &length,
                            PSTR("guest is already running"));
            shell_send_text(line, length);
        }
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_run_all) == 0)
    {
        if (!rv_demo_run_all())
        {
            uint8_t length = 0u;

            buffer_append_P(line, &length,
                            PSTR("no runnable guest"));
            shell_send_text(line, length);
        }
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_run_all_named) == 0)
    {
        if (!rv_demo_run_all())
        {
            uint8_t length = 0u;
            buffer_append_P(line, &length, PSTR("no runnable guest"));
            shell_send_text(line, length);
        }
    }
    else if (NODE_ROLE == 0 && (strcmp_P(line, command_guest_list) == 0 ||
             strcmp_P(line, command_guest_directory) == 0))
    {
        shell_send_guest_list(line);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_prefix_on) == 0)
    {
        rv_demo_set_output_prefix(1u);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_prefix_off) == 0)
    {
        rv_demo_set_output_prefix(0u);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_prefix_status) == 0)
    {
        uint8_t length = 0u;
        buffer_append_P(line, &length, PSTR("guest prefix="));
        buffer_append_P(line, &length,
                        rv_demo_output_prefix_enabled() ? PSTR("on")
                                                        : PSTR("off"));
        shell_send_text(line, length);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_trace_on) == 0)
    {
        rv_demo_set_switch_trace(1u);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_trace_off) == 0)
    {
        rv_demo_set_switch_trace(0u);
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_trace_status) == 0)
    {
        uint8_t length = 0u;
        buffer_append_P(line, &length, PSTR("guest trace="));
        buffer_append_P(line, &length,
                        rv_demo_switch_trace_enabled() ? PSTR("on sample=250")
                                                       : PSTR("off"));
        shell_send_text(line, length);
    }
    else if (strcmp_P(line, command_guest_info) == 0)
    {
        shell_send_guest_info(line);
    }
    else if (strcmp_P(line, command_guest_regs) == 0)
    {
        shell_send_guest_regs(line);
    }
    else if (NODE_ROLE == 0 && strcmp_P(line, command_guest_builtin) == 0)
    {
        shell_cancel_guest_load();
        rv_demo_use_builtin();
        shell_send_compressed(log_shell_ok_data, LOG_SHELL_OK_BYTE_COUNT,
                              LOG_SHELL_OK_BIT_COUNT);
    }
    else if (strncmp_P(line, command_guest_binary_load_prefix,
                       sizeof(command_guest_binary_load_prefix) - 1u) == 0)
    {
        char name[RV_DEMO_NAME_CAPACITY];
        uint16_t image_size = 0u;
        uint16_t expected_crc = 0u;
        uint16_t entry_point = 0u;
        const char *arguments =
            line + sizeof(command_guest_binary_load_prefix) - 1u;

        if (!shell_parse_binary_guest_load(arguments, name, &image_size,
                                           &expected_crc, &entry_point))
        {
            uint8_t message_length = 0u;

            buffer_append_P(line, &message_length,
                            PSTR("usage: guest bload <name> <size> <crc> <entry>"));
            shell_send_text(line, message_length);
        }
        else
        {
            shell_start_guest_load(line, name, image_size, expected_crc,
                                   entry_point);
            if (guest_load_session)
            {
                shell_receive_binary_guest(line);
            }
        }
    }
    else if (NODE_ROLE == 0 && strncmp_P(line, command_guest_prefix,
                       sizeof(command_guest_prefix) - 1u) == 0)
    {
        char name[RV_DEMO_NAME_CAPACITY];
        const char *arguments = line + sizeof(command_guest_prefix) - 1u;
        uint8_t length = 0u;

        if (!shell_parse_guest_name(&arguments, name))
        {
            buffer_append_P(line, &length,
                            PSTR("usage: guest <name> -run|-restart|-priority <0..3>"));
        }
        else if (strcmp(arguments, "-run") == 0 ||
                 strcmp(arguments, "-restart") == 0)
        {
            if (rv_demo_run_name(name))
            {
                return;
            }
            buffer_append_P(line, &length, PSTR("guest not found or busy"));
        }
        else if (strncmp(arguments, "-priority ", 10u) == 0 &&
                 arguments[10] >= '0' && arguments[10] <= '3' &&
                 arguments[11] == '\0')
        {
            if (rv_demo_set_priority(name,
                                     (uint8_t)(arguments[10] - '0')))
            {
                buffer_append_P(line, &length, PSTR("ok"));
            }
            else
            {
                buffer_append_P(line, &length,
                                PSTR("guest not found or busy"));
            }
        }
        else
        {
            buffer_append_P(line, &length,
                            PSTR("usage: guest <name> -run|-restart|-priority <0..3>"));
        }
        shell_send_text(line, length);
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
    uint8_t ignore_line_ending = 0u;
    uint8_t escape_state = 0u;
#if NODE_ROLE == 1
    uint16_t notified_id = 0;
    uint8_t notified_state = JOB_NONE;
#endif

#if NODE_ROLE == 1
    SHELL_TEXT("UNO controller ready. Type help.");
#elif NODE_ROLE == 2
    SHELL_TEXT("NANO worker ready. Type help.");
#else
    SHELL_TEXT("Local OS ready. Type help.");
#endif
    shell_send_prompt();

    for (;;)
    {
        char input;

        nodes_poll();
        nodes_service();
#if NODE_ROLE == 1
        /* Reuse the idle input buffer rather than allocate another SRAM line.
         * Defer while typing; never corrupt a partial command or binary upload.
         * Keep polling even after DONE so UNKNOWN can later resolve to DONE. */
        if (!length && !overflow && !escape_state)
        {
            job_info_t job = nodes_job();
            uint8_t terminal = job.state == JOB_DONE || job.state == JOB_FAULT ||
                               job.state == JOB_REJECTED || job.state == JOB_UNKNOWN ||
                               job.state == JOB_CANCELLED;
            if (job.id && terminal &&
                (job.id != notified_id || job.state != notified_state))
            {
                uart_write_char('\r');
                uart_write_char('\n');
                shell_show_job(line, job);
                notified_id = job.id;
                notified_state = job.state;
                shell_record_result(job);
                shell_finish_automation(line, job);
                shell_send_prompt();
            }
        }
#endif
#if NODE_ROLE
        if (uart_read_char_timeout(&input, 1u) != WAIT_ACQUIRED) continue;
#else
        uart_read_char_wait(&input);
#endif

        if ((uint8_t)input == 0x1bu)
        {
            /* An escape sequence is new user input, not the LF half of the
             * previous CRLF pair.  Otherwise Enter after history recall can
             * be mistaken for a duplicate line ending. */
            ignore_line_ending = 0u;
            escape_state = 1u;
            continue;
        }
        if (escape_state == 1u)
        {
            escape_state = (input == '[' || input == 'O') ? 2u : 0u;
            continue;
        }
        if (escape_state == 2u)
        {
            escape_state = 0u;
            continue;
        }

        if ((input == '\r' || input == '\n') && ignore_line_ending)
        {
            ignore_line_ending = 0u;
            continue;
        }
        ignore_line_ending = 0u;

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
            ignore_line_ending = 1u;
            shell_send_prompt();
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
