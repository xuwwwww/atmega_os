#include "event_flags.h"

#include "scheduler.h"

#include <avr/interrupt.h>
#include <avr/io.h>

_Static_assert(sizeof(event_flags_t) == 12u,
               "event_flags_t layout changed; verify SRAM budget");

static uint8_t event_flags_match(uint8_t available, uint8_t requested,
                                 uint8_t wait_all)
{
    uint8_t matched = available & requested;

    if (wait_all)
    {
        return matched == requested ? matched : 0u;
    }

    return matched;
}

static void event_flags_cancel_wait(void *object, uint8_t task_index)
{
    event_flags_t *events = object;
    uint8_t task_mask = (uint8_t)(1u << task_index);

    events->waiters &= (uint8_t)~task_mask;
    events->wait_all &= (uint8_t)~task_mask;
    events->auto_clear &= (uint8_t)~task_mask;
    events->masks[task_index] = 0u;
}

/* Interrupts must already be disabled, either by the caller or by ISR entry. */
static void event_flags_set_locked(event_flags_t *events, uint8_t mask)
{
    events->bits |= mask;

    for (uint8_t task_index = 0u;
         task_index < scheduler_task_count(); task_index++)
    {
        uint8_t task_mask = (uint8_t)(1u << task_index);
        uint8_t matched;

        if ((events->waiters & task_mask) == 0u)
        {
            continue;
        }

        matched = event_flags_match(
            events->bits, events->masks[task_index],
            (events->wait_all & task_mask) != 0u);
        if (matched == 0u)
        {
            continue;
        }

        events->waiters &= (uint8_t)~task_mask;
        events->wait_all &= (uint8_t)~task_mask;
        events->masks[task_index] = 0u;

        if ((events->auto_clear & task_mask) != 0u)
        {
            events->bits &= (uint8_t)~matched;
        }
        events->auto_clear &= (uint8_t)~task_mask;

        scheduler_complete_wait_value(task_index, WAIT_ACQUIRED, matched);
    }
}

static wait_result_t event_flags_wait_locked(event_flags_t *events,
                                             uint8_t mask,
                                             uint8_t options,
                                             uint32_t timeout_ms,
                                             uint8_t timed,
                                             uint8_t *matched_bits)
{
    uint8_t matched;
    uint8_t task_index;
    uint8_t task_mask;

    if (mask == 0u)
    {
        *matched_bits = 0u;
        return WAIT_ERROR;
    }

    matched = event_flags_match(events->bits, mask,
                                (options & EVENT_WAIT_ALL) != 0u);
    if (matched != 0u)
    {
        if ((options & EVENT_AUTO_CLEAR) != 0u)
        {
            events->bits &= (uint8_t)~matched;
        }
        *matched_bits = matched;
        return WAIT_ACQUIRED;
    }

    if (timed && timeout_ms == 0u)
    {
        *matched_bits = 0u;
        return WAIT_TIMEOUT;
    }

    task_index = scheduler_current_task_index();
    task_mask = (uint8_t)(1u << task_index);
    events->masks[task_index] = mask;
    events->waiters |= task_mask;

    if ((options & EVENT_WAIT_ALL) != 0u)
    {
        events->wait_all |= task_mask;
    }
    else
    {
        events->wait_all &= (uint8_t)~task_mask;
    }

    if ((options & EVENT_AUTO_CLEAR) != 0u)
    {
        events->auto_clear |= task_mask;
    }
    else
    {
        events->auto_clear &= (uint8_t)~task_mask;
    }

    if (timed)
    {
        scheduler_block_current_timeout(timeout_ms, events,
                                        event_flags_cancel_wait);
    }
    else
    {
        scheduler_block_current();
    }

    return WAIT_ERROR; /* Internal marker: the caller must context-switch. */
}

void event_flags_init(event_flags_t *events, uint8_t initial_bits)
{
    events->bits = initial_bits;
    events->waiters = 0u;
    events->wait_all = 0u;
    events->auto_clear = 0u;

    for (uint8_t i = 0u; i < SCHEDULER_MAX_TASKS; i++)
    {
        events->masks[i] = 0u;
    }
}

uint8_t event_flags_get(event_flags_t *events)
{
    uint8_t saved_sreg = SREG;
    uint8_t bits;

    cli();
    bits = events->bits;
    SREG = saved_sreg;
    return bits;
}

void event_flags_clear(event_flags_t *events, uint8_t mask)
{
    uint8_t saved_sreg = SREG;

    cli();
    events->bits &= (uint8_t)~mask;
    SREG = saved_sreg;
}

void event_flags_set(event_flags_t *events, uint8_t mask)
{
    uint8_t saved_sreg = SREG;

    cli();
    event_flags_set_locked(events, mask);
    SREG = saved_sreg;
}

void event_flags_set_from_isr(event_flags_t *events, uint8_t mask)
{
    event_flags_set_locked(events, mask);
}

uint8_t event_flags_try_wait(event_flags_t *events, uint8_t mask,
                             uint8_t options, uint8_t *matched_bits)
{
    uint8_t saved_sreg = SREG;
    uint8_t matched;

    cli();
    matched = event_flags_match(events->bits, mask,
                                (options & EVENT_WAIT_ALL) != 0u);
    if (matched != 0u && (options & EVENT_AUTO_CLEAR) != 0u)
    {
        events->bits &= (uint8_t)~matched;
    }
    SREG = saved_sreg;

    *matched_bits = matched;
    return matched != 0u;
}

wait_result_t event_flags_wait(event_flags_t *events, uint8_t mask,
                               uint8_t options, uint8_t *matched_bits)
{
    uint8_t saved_sreg = SREG;
    wait_result_t result;

    cli();
    result = event_flags_wait_locked(events, mask, options, 0u, 0u,
                                     matched_bits);
    SREG = saved_sreg;

    if (result != WAIT_ERROR || mask == 0u)
    {
        return result;
    }

    task_yield();
    result = scheduler_current_wait_result();
    *matched_bits = result == WAIT_ACQUIRED
                        ? scheduler_current_wait_value()
                        : 0u;
    return result;
}

wait_result_t event_flags_wait_timeout(event_flags_t *events, uint8_t mask,
                                       uint8_t options, uint32_t timeout_ms,
                                       uint8_t *matched_bits)
{
    uint8_t saved_sreg = SREG;
    wait_result_t result;

    cli();
    result = event_flags_wait_locked(events, mask, options, timeout_ms, 1u,
                                     matched_bits);
    SREG = saved_sreg;

    if (result != WAIT_ERROR || mask == 0u || timeout_ms == 0u)
    {
        return result;
    }

    task_yield();
    result = scheduler_current_wait_result();
    *matched_bits = result == WAIT_ACQUIRED
                        ? scheduler_current_wait_value()
                        : 0u;
    return result;
}
