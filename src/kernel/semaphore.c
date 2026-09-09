#include "semaphore.h"

#include "scheduler.h"

#include <avr/interrupt.h>
#include <avr/io.h>

static void semaphore_cancel_wait(void *object, uint8_t task_index)
{
    semaphore_t *semaphore = object;
    semaphore->waiters &= (uint8_t)~(1u << task_index);
}

static uint8_t semaphore_wake_one(semaphore_t *semaphore)
{
    for (uint8_t task_index = 0; task_index < 8u; task_index++)
    {
        uint8_t mask = (uint8_t)(1u << task_index);

        if ((semaphore->waiters & mask) != 0u)
        {
            semaphore->waiters &= (uint8_t)~mask;
            scheduler_complete_wait(task_index, WAIT_ACQUIRED);
            return 1;
        }
    }

    return 0;
}

void semaphore_init(semaphore_t *semaphore, uint8_t initial_count,
                    uint8_t max_count)
{
    semaphore->waiters = 0;
    semaphore->max_count = max_count;
    semaphore->count = initial_count <= max_count ? initial_count : max_count;
}

void semaphore_take(semaphore_t *semaphore)
{
    uint8_t saved_sreg = SREG;

    cli();

    if (semaphore->count > 0u)
    {
        semaphore->count--;
        SREG = saved_sreg;
        return;
    }

    semaphore->waiters |=
        (uint8_t)(1u << scheduler_current_task_index());
    scheduler_block_current();
    SREG = saved_sreg;

    task_yield();
}

wait_result_t semaphore_take_timeout(semaphore_t *semaphore,
                                     uint32_t timeout_ms)
{
    uint8_t saved_sreg = SREG;

    cli();

    if (semaphore->count > 0u)
    {
        semaphore->count--;
        SREG = saved_sreg;
        return WAIT_ACQUIRED;
    }

    if (timeout_ms == 0u)
    {
        SREG = saved_sreg;
        return WAIT_TIMEOUT;
    }

    semaphore->waiters |=
        (uint8_t)(1u << scheduler_current_task_index());
    scheduler_block_current_timeout(timeout_ms, semaphore,
                                    semaphore_cancel_wait);
    SREG = saved_sreg;

    task_yield();
    return scheduler_current_wait_result();
}

uint8_t semaphore_try_take(semaphore_t *semaphore)
{
    uint8_t saved_sreg = SREG;
    uint8_t taken = 0;

    cli();
    if (semaphore->count > 0u)
    {
        semaphore->count--;
        taken = 1;
    }
    SREG = saved_sreg;

    return taken;
}

uint8_t semaphore_give(semaphore_t *semaphore)
{
    uint8_t saved_sreg = SREG;
    uint8_t given = 1;

    cli();
    if (!semaphore_wake_one(semaphore))
    {
        if (semaphore->count < semaphore->max_count)
        {
            semaphore->count++;
        }
        else
        {
            given = 0;
        }
    }
    SREG = saved_sreg;

    return given;
}

void semaphore_give_from_isr(semaphore_t *semaphore)
{
    if (!semaphore_wake_one(semaphore) &&
        semaphore->count < semaphore->max_count)
    {
        semaphore->count++;
    }
}
