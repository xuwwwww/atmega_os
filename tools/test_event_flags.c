#include "event_flags.h"

#include <assert.h>
#include <stdio.h>

uint8_t SREG = 0x80u;

static uint8_t mock_current_task;
static wait_result_t mock_result;
static uint8_t mock_value;
static void (*yield_action)(void);
static void *timeout_object;
static task_wait_cancel_t timeout_cancel;

void host_cli(void)
{
    SREG &= (uint8_t)~0x80u;
}

uint8_t scheduler_task_count(void)
{
    return 4u;
}

uint8_t scheduler_current_task_index(void)
{
    return mock_current_task;
}

void scheduler_block_current(void)
{
}

void scheduler_block_current_timeout(uint32_t timeout_ms, void *object,
                                     task_wait_cancel_t cancel)
{
    assert(timeout_ms > 0u);
    timeout_object = object;
    timeout_cancel = cancel;
}

void scheduler_complete_wait_value(uint8_t task_index, wait_result_t result,
                                   uint8_t value)
{
    assert(task_index == mock_current_task);
    mock_result = result;
    mock_value = value;
}

void task_yield(void)
{
    assert(yield_action != NULL);
    yield_action();
}

wait_result_t scheduler_current_wait_result(void)
{
    return mock_result;
}

uint8_t scheduler_current_wait_value(void)
{
    return mock_value;
}

static event_flags_t test_events;

static void set_bits_while_blocked(void)
{
    event_flags_set_from_isr(&test_events, 0x06u);
}

static void timeout_while_blocked(void)
{
    assert(timeout_cancel != NULL);
    timeout_cancel(timeout_object, mock_current_task);
    mock_result = WAIT_TIMEOUT;
    mock_value = 0u;
}

int main(void)
{
    uint8_t matched = 0u;

    event_flags_init(&test_events, 0x03u);
    assert(event_flags_get(&test_events) == 0x03u);

    assert(event_flags_try_wait(&test_events, 0x02u, EVENT_WAIT_ANY,
                                &matched));
    assert(matched == 0x02u);
    assert(event_flags_get(&test_events) == 0x03u);

    assert(event_flags_try_wait(&test_events, 0x03u,
                                EVENT_WAIT_ALL | EVENT_AUTO_CLEAR,
                                &matched));
    assert(matched == 0x03u);
    assert(event_flags_get(&test_events) == 0u);

    mock_current_task = 2u;
    yield_action = set_bits_while_blocked;
    assert(event_flags_wait(&test_events, 0x06u,
                            EVENT_WAIT_ALL | EVENT_AUTO_CLEAR,
                            &matched) == WAIT_ACQUIRED);
    assert(matched == 0x06u);
    assert(event_flags_get(&test_events) == 0u);

    yield_action = timeout_while_blocked;
    assert(event_flags_wait_timeout(&test_events, 0x80u,
                                    EVENT_WAIT_ANY, 5u,
                                    &matched) == WAIT_TIMEOUT);
    assert(matched == 0u);
    assert(test_events.waiters == 0u);

    event_flags_set(&test_events, 0x55u);
    event_flags_clear(&test_events, 0x05u);
    assert(event_flags_get(&test_events) == 0x50u);

    puts("event flags tests passed");
    return 0;
}
