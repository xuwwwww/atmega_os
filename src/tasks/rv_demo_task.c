#include "rv_demo_task.h"

#include "../drivers/uart.h"
#include "../generated/rv_guest_image.h"
#include "../kernel/scheduler.h"
#include "../kernel/timer.h"
#include "../kernel/nodes.h"
#include "../vm/rv32i.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RV_DEMO_MEMORY_SIZE 192u
#define RV_DEMO_TRACE_PERIOD_SLICES 250u
#define RV_DEMO_TIMER1_COUNTS_PER_SLICE (F_CPU / (64UL * 1000UL))

#if (F_CPU % (64UL * 1000UL)) != 0UL || \
    RV_DEMO_TIMER1_COUNTS_PER_SLICE > UINT16_MAX
#error "Timer1 /64 must represent an exact 1 ms guest slice"
#endif

#ifndef RV_GUEST_SLOT_COUNT
#define RV_GUEST_SLOT_COUNT 1u
#endif

#if RV_GUEST_SLOT_COUNT < 1 || RV_GUEST_SLOT_COUNT > 8
#error "RV_GUEST_SLOT_COUNT must be between 1 and 8"
#endif

#define RV_DEMO_SLOT_COUNT RV_GUEST_SLOT_COUNT

enum
{
    RV_SYSCALL_YIELD = 0,
    RV_SYSCALL_SLEEP = 1,
    RV_SYSCALL_WRITE = 2,
    RV_SYSCALL_EXIT = 3,
};

typedef struct __attribute__((aligned(4)))
{
    uint32_t syscall_number;
    uint32_t argument0;
    uint32_t argument1;
    uint32_t exit_code;
} rv_demo_context_t;

typedef struct
{
    rv32_cpu_t cpu;
    rv_demo_context_t context;
    uint8_t memory[RV_DEMO_MEMORY_SIZE];
    char name[RV_DEMO_NAME_CAPACITY];
    uint16_t uploaded_size;
    uint16_t expected_size;
    uint16_t sleep_remaining_ms;
    uint8_t uploaded_valid;
    uint8_t upload_in_progress;
    uint8_t priority;
} rv_demo_slot_t;

_Static_assert(sizeof(rv_demo_slot_t) % 4u == 0u,
               "every VM slot must preserve 32-bit register alignment");
_Static_assert(sizeof(rv_demo_slot_t) == 376u,
               "directory metadata must not grow the hardware-tested VM slot");

/* The compatibility layer serializes guest registers as bytes, so correctness
 * no longer depends on AVR uint32_t alignment.  Keeping every slot 4-byte
 * aligned still gives its context fields and pointer arithmetic one stable
 * layout across host tests and the embedded build. */
static rv_demo_slot_t slots[RV_DEMO_SLOT_COUNT];
/* Directory metadata is deliberately kept outside rv_demo_slot_t.  The VM
 * slot has a hardware-tested 376-byte stride on this LGT8F clone; growing it
 * moved slot 1/2 and made otherwise valid RV32 execution layout-sensitive. */
static uint16_t slot_image_crc[RV_DEMO_SLOT_COUNT];
static uint16_t slot_entry_point[RV_DEMO_SLOT_COUNT];
static volatile uint8_t guest_active_mask;
static volatile uint8_t cancel_requested;
static uint8_t last_run_cancelled;
static uint8_t output_prefix_enabled;
static uint8_t switch_trace_enabled;

static const char single_exit_prefix[] PROGMEM = "rv32 C guest exit=";
static const char slot_prefix[] PROGMEM = "rv32 ";
static const char switch_prefix[] PROGMEM = "[switch -> ";

static uint16_t memory_crc16(const uint8_t *memory, uint16_t length)
{
    uint16_t crc = 0xffffu;

    for (uint16_t i = 0u; i < length; i++)
    {
        crc ^= (uint16_t)memory[i] << 8u;
        for (uint8_t bit = 0u; bit < 8u; bit++)
        {
            crc = (crc & 0x8000u) != 0u
                      ? (uint16_t)((crc << 1u) ^ 0x1021u)
                      : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

static void send_switch_trace(uint8_t index)
{
    char message[25];
    uint8_t length = 0u;

    for (uint8_t i = 0u; i < sizeof(switch_prefix) - 1u; i++)
    {
        message[length++] = (char)pgm_read_byte(&switch_prefix[i]);
    }
    for (uint8_t i = 0u;
         i < RV_DEMO_NAME_CAPACITY && slots[index].name[i] != '\0'; i++)
    {
        message[length++] = slots[index].name[i];
    }
    message[length++] = ']';
    uart_write_binary_frame(UART_FRAME_SHELL_TEXT,
                            (const uint8_t *)message, length);
}

static uint8_t slot_valid(uint8_t slot)
{
    return slot < RV_DEMO_SLOT_COUNT;
}

static uint8_t claim_guest_slots(uint8_t mask)
{
    uint8_t saved_sreg = SREG;
    uint8_t claimed = 0u;
    uint8_t upload_active = 0u;

    cli();
    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        upload_active |= slots[index].upload_in_progress;
    }
    if (guest_active_mask == 0u && upload_active == 0u)
    {
        guest_active_mask = mask;
        claimed = 1u;
    }
    SREG = saved_sreg;
    return claimed;
}

static void release_guest_slots(uint8_t mask)
{
    uint8_t saved_sreg = SREG;

    cli();
    guest_active_mask &= (uint8_t)~mask;
    SREG = saved_sreg;
}

static void send_result(uint8_t index, uint8_t success, uint32_t value)
{
    char message[52];
    uint8_t length = 0u;

    if (index == UINT8_MAX)
    {
        for (uint8_t i = 0u; i < sizeof(single_exit_prefix) - 1u; i++)
        {
            message[length++] =
                (char)pgm_read_byte(&single_exit_prefix[i]);
        }
    }
    else
    {
        for (uint8_t i = 0u; i < sizeof(slot_prefix) - 1u; i++)
        {
            message[length++] = (char)pgm_read_byte(&slot_prefix[i]);
        }
        for (uint8_t i = 0u;
             i < RV_DEMO_NAME_CAPACITY && slots[index].name[i] != '\0'; i++)
        {
            message[length++] = slots[index].name[i];
        }
        if (success)
        {
            memcpy(&message[length], " exit=", 6u);
            length += 6u;
        }
        else
        {
            memcpy(&message[length], " fault=", 7u);
            length += 7u;
        }
    }

    ultoa(value, &message[length], 10);
    length = (uint8_t)strlen(message);
    if (!success && index != UINT8_MAX)
    {
        memcpy(&message[length], " pc=", 4u);
        length += 4u;
        ultoa(slots[index].cpu.pc, &message[length], 10);
    }
    uart_write_binary_frame(UART_FRAME_SHELL_TEXT,
                            (const uint8_t *)message,
                            (uint8_t)strlen(message));
}

static void append_hex16(char *buffer, uint8_t *length, uint16_t value)
{
    static const char digits[] PROGMEM = "0123456789abcdef";

    for (int8_t shift = 12; shift >= 0; shift -= 4)
    {
        buffer[(*length)++] =
            (char)pgm_read_byte(&digits[(value >> shift) & 0x0fu]);
    }
}

/* Keep the execution evidence next to every result.  Besides detecting guest
 * self-corruption, this diagnostic deliberately preserves the code layout of
 * the last firmware that repeatedly passed all three long workloads on the
 * LGT8F-compatible board. */
static void send_integrity(uint8_t index)
{
    rv_demo_slot_t *slot = &slots[index];
    char message[50];
    uint8_t length = 0u;
    uint16_t actual_crc = memory_crc16(slot->memory, slot->uploaded_size);

    memcpy(&message[length], "verify ", 7u);
    length += 7u;
    for (uint8_t i = 0u;
         i < RV_DEMO_NAME_CAPACITY && slot->name[i] != '\0'; i++)
    {
        message[length++] = slot->name[i];
    }
    memcpy(&message[length], " ptr=", 5u);
    length += 5u;
    message[length++] = slot->cpu.memory == slot->memory ? '1' : '0';
    memcpy(&message[length], " size=", 6u);
    length += 6u;
    utoa(slot->cpu.memory_size, &message[length], 10);
    length = (uint8_t)strlen(message);
    memcpy(&message[length], " crc=", 5u);
    length += 5u;
    append_hex16(message, &length, actual_crc);
    message[length++] = '/';
    append_hex16(message, &length, slot_image_crc[index]);
    uart_write_binary_frame(UART_FRAME_SHELL_TEXT,
                            (const uint8_t *)message, length);
}

static rv32_status_t handle_ecall(struct rv32_cpu *cpu, void *raw_context)
{
    rv_demo_context_t *context = raw_context;

    context->syscall_number = rv32_register_read(cpu, 17u); /* a7 */
    context->argument0 = rv32_register_read(cpu, 10u);      /* a0 */
    context->argument1 = rv32_register_read(cpu, 11u);      /* a1 */

    if (context->syscall_number <= RV_SYSCALL_WRITE)
    {
        return RV32_PAUSED;
    }
    if (context->syscall_number == RV_SYSCALL_EXIT)
    {
        context->exit_code = context->argument0;
        return RV32_HALTED;
    }
    return RV32_FAULT_ILLEGAL_INSTRUCTION;
}

static void process_syscall(rv_demo_slot_t *slot, uint8_t index,
                            uint8_t multiplexed)
{
    switch (slot->context.syscall_number)
    {
    case RV_SYSCALL_YIELD:
        rv32_register_write(&slot->cpu, 10u, 0u);
        slot->cpu.status = RV32_RUNNING;
        if (!multiplexed)
        {
            task_yield();
        }
        break;

    case RV_SYSCALL_SLEEP:
        rv32_register_write(&slot->cpu, 10u, 0u);
        slot->cpu.status = RV32_RUNNING;
        if (multiplexed)
        {
            slot->sleep_remaining_ms =
                slot->context.argument0 > UINT16_MAX
                    ? UINT16_MAX
                    : (uint16_t)slot->context.argument0;
        }
        else
        {
            task_sleep(slot->context.argument0);
        }
        break;

    case RV_SYSCALL_WRITE:
        if (slot->context.argument0 > RV_DEMO_MEMORY_SIZE ||
            slot->context.argument1 > UINT8_MAX ||
            slot->context.argument1 >
                (uint32_t)RV_DEMO_MEMORY_SIZE - slot->context.argument0)
        {
            slot->cpu.status = RV32_FAULT_LOAD;
            break;
        }
        if (output_prefix_enabled)
        {
            uart_write_named_binary_frame(
                UART_FRAME_SHELL_TEXT, slots[index].name,
                (uint8_t)strlen(slots[index].name),
                &slot->memory[(uint16_t)slot->context.argument0],
                (uint8_t)slot->context.argument1);
        }
        else
        {
            uart_write_binary_frame(
                UART_FRAME_SHELL_TEXT,
                &slot->memory[(uint16_t)slot->context.argument0],
                (uint8_t)slot->context.argument1);
        }
        rv32_register_write(&slot->cpu, 10u, slot->context.argument1);
        slot->cpu.status = RV32_RUNNING;
        break;

    default:
        slot->cpu.status = RV32_FAULT_ILLEGAL_INSTRUCTION;
        break;
    }
}

static uint8_t prepare_slot(uint8_t index)
{
    rv_demo_slot_t *slot = &slots[index];

    if (slot->uploaded_valid)
    {
        /* Execute the uploaded image in place. Stack contents and writable
         * data persist across runs; re-upload for a completely clean image. */
    }
    else if (index == 0u)
    {
        for (uint16_t i = 0u; i < RV_GUEST_IMAGE_SIZE; i++)
        {
            slot->memory[i] = pgm_read_byte(&rv_guest_image[i]);
        }
        memcpy(slot->name, "builtin", 8u);
        slot->uploaded_size = RV_GUEST_IMAGE_SIZE;
        slot->uploaded_valid = 1u;
        slot->priority = TASK_PRIORITY_NORMAL;
        slot_image_crc[index] =
            memory_crc16(slot->memory, slot->uploaded_size);
        slot_entry_point[index] = 0u;
    }
    else
    {
        return 0u;
    }

    memset(&slot->context, 0, sizeof(slot->context));
    slot->sleep_remaining_ms = 0u;
    rv32_init(&slot->cpu, slot->memory, sizeof(slot->memory));
    slot->cpu.pc = slot_entry_point[index];
    slot->cpu.ecall_handler = handle_ecall;
    slot->cpu.ecall_context = &slot->context;
    return 1u;
}

static uint8_t run_slots(uint8_t requested_mask)
{
    uint8_t running_mask = requested_mask;
    uint8_t cursor = 0u;
    uint8_t trace_countdown = 0u;
    uint8_t last_traced = UINT8_MAX;

    if (requested_mask == 0u || !claim_guest_slots(requested_mask))
    {
        return 0u;
    }
    cancel_requested = 0u;
    last_run_cancelled = 0u;

    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if ((running_mask & (uint8_t)(1u << index)) != 0u &&
            !prepare_slot(index))
        {
            running_mask &= (uint8_t)~(1u << index);
            release_guest_slots((uint8_t)(1u << index));
        }
    }

    /* Timer1 is intentionally active only while guest code is executing.
     * No Timer1 interrupt is enabled: TCNT1 is polled after each complete
     * RV32I instruction, so guest state is always consistent at a switch. */
    TCCR1A = 0u;
    TCCR1B = 0u;
    TCNT1 = 0u;
    TIFR1 = 0xffu;
    TIMSK1 = 0u;
    TCCR1B = (1u << CS11) | (1u << CS10);

    while (running_mask != 0u)
    {
        nodes_poll();
        {
            char input;
            if (uart_read_char(&input) && (input == 'q' || input == 'Q'))
                cancel_requested = 1u; /* q works through the WSL monitor filter. */
        }
        if (cancel_requested)
        {
            last_run_cancelled = 1u;
            release_guest_slots(running_mask);
            running_mask = 0u;
            break;
        }
        uint8_t selected = UINT8_MAX;
        uint8_t best_priority = 0u;

        /* Some LGT-compatible bootloaders leave their watchdog path active
         * even after the standard avr-libc disable sequence. Short demos used
         * to finish before its roughly two-second deadline; CPU-bound guests
         * expose it deterministically. Keep long VM runs alive here without
         * coupling watchdog service to every UART operation. */
        wdt_reset();

        for (uint8_t offset = 0u; offset < RV_DEMO_SLOT_COUNT; offset++)
        {
            uint8_t index = (uint8_t)((cursor + offset) % RV_DEMO_SLOT_COUNT);
            rv_demo_slot_t *slot = &slots[index];
            uint8_t bit = (uint8_t)(1u << index);

            if ((running_mask & bit) == 0u)
            {
                continue;
            }
            if (slot->sleep_remaining_ms != 0u)
            {
                slot->sleep_remaining_ms--;
                continue;
            }
            if (selected == UINT8_MAX || slot->priority > best_priority)
            {
                selected = index;
                best_priority = slot->priority;
            }
        }

        if (selected != UINT8_MAX)
        {
            rv_demo_slot_t *slot = &slots[selected];
            uint8_t bit = (uint8_t)(1u << selected);

            cursor = (uint8_t)((selected + 1u) % RV_DEMO_SLOT_COUNT);
            if (switch_trace_enabled && trace_countdown == 0u &&
                selected != last_traced)
            {
                send_switch_trace(selected);
                last_traced = selected;
                trace_countdown = RV_DEMO_TRACE_PERIOD_SLICES;
            }
            {
                uint16_t slice_started_at = TCNT1;

                do
                {
                    rv32_step(&slot->cpu);
                } while (slot->cpu.status == RV32_RUNNING &&
                         (uint16_t)(TCNT1 - slice_started_at) <
                             (uint16_t)RV_DEMO_TIMER1_COUNTS_PER_SLICE);
            }
            if (slot->cpu.status == RV32_PAUSED)
            {
                process_syscall(slot, selected, 1u);
            }
            if (slot->cpu.status != RV32_RUNNING)
            {
                running_mask &= (uint8_t)~bit;
                release_guest_slots(bit);
                send_result(selected, slot->cpu.status == RV32_HALTED,
                            slot->cpu.status == RV32_HALTED
                                ? slot->context.exit_code
                                : (uint32_t)slot->cpu.status);
                send_integrity(selected);
            }
        }
        if (trace_countdown != 0u)
        {
            trace_countdown--;
        }
        if (selected == UINT8_MAX)
        {
            timer_delay_ms(1u);
        }
    }
    TCCR1B = 0u;
    release_guest_slots(requested_mask);
    return 1u;
}

uint8_t rv_demo_run(void)
{
    if (!slots[0].uploaded_valid && !prepare_slot(0u))
    {
        return 0u;
    }
    return run_slots(1u);
}

uint8_t rv_demo_run_all(void)
{
    uint8_t mask = 0u;

    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if (slots[index].uploaded_valid)
        {
            mask |= (uint8_t)(1u << index);
        }
    }
    return run_slots(mask);
}

uint8_t rv_demo_run_name(const char *name)
{
    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if (slots[index].uploaded_valid &&
            strncmp(slots[index].name, name, RV_DEMO_NAME_CAPACITY) == 0)
        {
            return run_slots((uint8_t)(1u << index));
        }
    }
    return 0u;
}

void rv_demo_task(void) { (void)rv_demo_run(); }
uint8_t rv_demo_is_active(void) { return guest_active_mask != 0u; }
void rv_demo_request_cancel(void) { cancel_requested = 1u; }
uint8_t rv_demo_was_cancelled(void) { return last_run_cancelled; }
uint8_t rv_demo_status(void) { return (uint8_t)slots[0].cpu.status; }
uint16_t rv_demo_image_size(void)
{
    return slots[0].uploaded_valid ? slots[0].uploaded_size
                                   : RV_GUEST_IMAGE_SIZE;
}
uint16_t rv_demo_memory_size(void) { return RV_DEMO_MEMORY_SIZE; }
uint32_t rv_demo_exit_code(void) { return slots[0].context.exit_code; }
uint32_t rv_demo_pc(void) { return slots[0].cpu.pc; }
uint32_t rv_demo_register(uint8_t index)
{
    return rv32_register_read(&slots[0].cpu, index);
}

uint8_t rv_demo_slot_count(void) { return RV_DEMO_SLOT_COUNT; }
uint8_t rv_demo_slot_loaded(uint8_t index)
{
    return slot_valid(index) && slots[index].uploaded_valid;
}
int8_t rv_demo_find_name(const char *name)
{
    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if (slots[index].uploaded_valid &&
            strncmp(slots[index].name, name, RV_DEMO_NAME_CAPACITY) == 0)
        {
            return (int8_t)index;
        }
    }
    return -1;
}
uint16_t rv_demo_storage_bytes(void) { return sizeof(slots); }
const char *rv_demo_slot_name(uint8_t index)
{
    return slot_valid(index) ? slots[index].name : "";
}
uint8_t rv_demo_slot_priority(uint8_t index)
{
    return slot_valid(index) ? slots[index].priority : 0u;
}
uint16_t rv_demo_slot_image_size(uint8_t index)
{
    return slot_valid(index) ? slots[index].uploaded_size : 0u;
}
uint16_t rv_demo_slot_image_crc(uint8_t index)
{
    return slot_valid(index) ? slot_image_crc[index] : 0u;
}
uint16_t rv_demo_slot_entry_point(uint8_t index)
{
    return slot_valid(index) ? slot_entry_point[index] : 0u;
}
uint8_t rv_demo_slot_status(uint8_t index)
{
    return slot_valid(index) ? (uint8_t)slots[index].cpu.status : 7u;
}
uint32_t rv_demo_slot_exit(uint8_t index)
{
    return slot_valid(index) ? slots[index].context.exit_code : 0u;
}
uint8_t rv_demo_slot_image_byte(uint8_t index, uint16_t offset)
{
    return slot_valid(index) && slots[index].uploaded_valid &&
           offset < slots[index].uploaded_size ? slots[index].memory[offset] : 0u;
}
void rv_demo_register_builtin(void)
{
    if (!slots[0].uploaded_valid) (void)prepare_slot(0u);
}

uint8_t rv_demo_set_priority(const char *name, uint8_t priority)
{
    if (priority > TASK_PRIORITY_HIGH || guest_active_mask != 0u)
    {
        return 0u;
    }
    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if (slots[index].uploaded_valid &&
            strncmp(slots[index].name, name, RV_DEMO_NAME_CAPACITY) == 0)
        {
            slots[index].priority = priority;
            return 1u;
        }
    }
    return 0u;
}

void rv_demo_set_output_prefix(uint8_t enabled)
{
    output_prefix_enabled = enabled != 0u;
}

uint8_t rv_demo_output_prefix_enabled(void)
{
    return output_prefix_enabled;
}

void rv_demo_set_switch_trace(uint8_t enabled)
{
    switch_trace_enabled = enabled != 0u;
}

uint8_t rv_demo_switch_trace_enabled(void)
{
    return switch_trace_enabled;
}

int8_t rv_demo_upload_begin_name(const char *name, uint16_t image_size)
{
    int8_t free_slot = -1;
    uint8_t existing = 0u;
    size_t length = strlen(name);

    if (length == 0u || length >= RV_DEMO_NAME_CAPACITY)
    {
        return -1;
    }
    for (uint8_t index = 0u; index < RV_DEMO_SLOT_COUNT; index++)
    {
        if (slots[index].uploaded_valid &&
            strncmp(slots[index].name, name, RV_DEMO_NAME_CAPACITY) == 0)
        {
            free_slot = (int8_t)index;
            existing = 1u;
            break;
        }
        if (free_slot < 0 && !slots[index].uploaded_valid &&
            !slots[index].upload_in_progress)
        {
            free_slot = (int8_t)index;
        }
    }
    if (free_slot < 0 ||
        !rv_demo_upload_begin_slot((uint8_t)free_slot, image_size))
    {
        return -1;
    }
    memset(slots[(uint8_t)free_slot].name, 0, RV_DEMO_NAME_CAPACITY);
    memcpy(slots[(uint8_t)free_slot].name, name, length);
    if (!existing)
    {
        slots[(uint8_t)free_slot].priority = TASK_PRIORITY_NORMAL;
    }
    return free_slot;
}

int8_t rv_demo_upload_begin_cached(const char *name, uint16_t image_size)
{
    int8_t slot = rv_demo_upload_begin_name(name, image_size);
    if (slot >= 0 || guest_active_mask != 0u) return slot;
    static uint8_t victim;
    uint8_t first = RV_DEMO_SLOT_COUNT > 1u ? 1u : 0u;
    uint8_t count = (uint8_t)(RV_DEMO_SLOT_COUNT - first);
    uint8_t index = (uint8_t)(first + victim++ % count);
    if (!rv_demo_upload_begin_slot(index, image_size)) return -1;
    memset(slots[index].name, 0, RV_DEMO_NAME_CAPACITY);
    strncpy(slots[index].name, name, RV_DEMO_NAME_CAPACITY - 1u);
    slots[index].priority = TASK_PRIORITY_NORMAL;
    return (int8_t)index;
}

uint8_t rv_demo_upload_begin_slot(uint8_t index, uint16_t image_size)
{
    rv_demo_slot_t *slot;
    uint8_t saved_sreg;

    if (!slot_valid(index) || image_size == 0u ||
        image_size > RV_DEMO_MEMORY_SIZE)
    {
        return 0u;
    }

    saved_sreg = SREG;
    cli();
    if (guest_active_mask != 0u)
    {
        SREG = saved_sreg;
        return 0u;
    }
    slot = &slots[index];
    slot->uploaded_size = 0u;
    slot->uploaded_valid = 0u;
    slot->expected_size = image_size;
    slot->upload_in_progress = 1u;
    SREG = saved_sreg;
    return 1u;
}

uint8_t rv_demo_upload_write_slot(uint8_t index, uint16_t offset,
                                  uint8_t value)
{
    rv_demo_slot_t *slot;

    if (!slot_valid(index))
    {
        return 0u;
    }
    slot = &slots[index];
    if (!slot->upload_in_progress || offset >= slot->expected_size)
    {
        return 0u;
    }
    slot->memory[offset] = value;
    return 1u;
}

uint16_t rv_demo_upload_crc16_slot(uint8_t index, uint16_t length)
{
    rv_demo_slot_t *slot;

    if (!slot_valid(index))
    {
        return 0u;
    }
    slot = &slots[index];
    if (!slot->upload_in_progress || length > slot->expected_size)
    {
        return 0u;
    }
    return memory_crc16(slot->memory, length);
}

uint8_t rv_demo_upload_commit_slot(uint8_t index)
{
    uint16_t crc;

    if (!slot_valid(index))
    {
        return 0u;
    }
    crc = memory_crc16(slots[index].memory, slots[index].expected_size);
    return rv_demo_upload_commit_metadata_slot(index, crc, 0u);
}

uint8_t rv_demo_upload_commit_metadata_slot(uint8_t index,
                                            uint16_t image_crc,
                                            uint16_t entry_point)
{
    rv_demo_slot_t *slot;

    if (!slot_valid(index))
    {
        return 0u;
    }
    slot = &slots[index];
    if (!slot->upload_in_progress || entry_point >= slot->expected_size ||
        (entry_point & 3u) != 0u)
    {
        return 0u;
    }
    slot->uploaded_size = slot->expected_size;
    slot_image_crc[index] = image_crc;
    slot_entry_point[index] = entry_point;
    slot->uploaded_valid = 1u;
    slot->upload_in_progress = 0u;
    return 1u;
}

void rv_demo_upload_cancel_slot(uint8_t index)
{
    if (slot_valid(index))
    {
        slots[index].expected_size = 0u;
        slots[index].upload_in_progress = 0u;
    }
}

uint8_t rv_demo_upload_begin(uint16_t size)
{
    return rv_demo_upload_begin_slot(0u, size);
}
uint8_t rv_demo_upload_write(uint16_t offset, uint8_t value)
{
    return rv_demo_upload_write_slot(0u, offset, value);
}
uint16_t rv_demo_upload_crc16(uint16_t length)
{
    return rv_demo_upload_crc16_slot(0u, length);
}
uint8_t rv_demo_upload_commit(void)
{
    return rv_demo_upload_commit_slot(0u);
}
void rv_demo_upload_cancel(void) { rv_demo_upload_cancel_slot(0u); }
void rv_demo_use_builtin(void)
{
    rv_demo_upload_cancel_slot(0u);
    slots[0].uploaded_valid = 0u;
    (void)prepare_slot(0u);
}
uint8_t rv_demo_image_is_uploaded(void) { return slots[0].uploaded_valid; }
