#include "mutex.h"

#include "scheduler.h"

#include <avr/interrupt.h>
#include <avr/io.h>

_Static_assert(sizeof(mutex_t) == 4u,
               "mutex_t layout changed; verify SRAM budget");

static mutex_t *g_mutexes = 0;

static void mutex_recompute_all_priorities(void)
{
    uint8_t task_count = scheduler_task_count();

    /* First remove inheritance that is no longer justified. */
    for (uint8_t task_index = 0; task_index < task_count; task_index++)
    {
        scheduler_set_effective_priority(
            task_index, scheduler_task_base_priority(task_index));
    }

    /* One pass propagates one mutex edge; task_count passes cover any chain. */
    for (uint8_t pass = 0; pass < task_count; pass++)
    {
        uint8_t changed = 0u;

        for (mutex_t *mutex = g_mutexes; mutex != 0; mutex = mutex->next)
        {
            if (mutex->owner == MUTEX_NO_OWNER)
            {
                continue;
            }

            task_priority_t owner_priority =
                scheduler_task_effective_priority(mutex->owner);

            for (uint8_t waiter = 0; waiter < task_count; waiter++)
            {
                uint8_t mask = (uint8_t)(1u << waiter);

                if ((mutex->waiters & mask) != 0u)
                {
                    task_priority_t waiter_priority =
                        scheduler_task_effective_priority(waiter);

                    if (waiter_priority > owner_priority)
                    {
                        owner_priority = waiter_priority;
                    }
                }
            }

            if (owner_priority >
                scheduler_task_effective_priority(mutex->owner))
            {
                scheduler_set_effective_priority(mutex->owner,
                                                 owner_priority);
                changed = 1u;
            }
        }

        if (!changed)
        {
            break;
        }
    }
}

static void mutex_cancel_wait(void *object, uint8_t task_index)
{
    mutex_t *mutex = object;

    mutex->waiters &= (uint8_t)~(1u << task_index);
    mutex_recompute_all_priorities();
}

static uint8_t mutex_handoff(mutex_t *mutex)
{
    uint8_t selected = MUTEX_NO_OWNER;
    task_priority_t selected_priority = TASK_PRIORITY_IDLE;

    for (uint8_t task_index = 0; task_index < scheduler_task_count();
         task_index++)
    {
        uint8_t mask = (uint8_t)(1u << task_index);

        if ((mutex->waiters & mask) != 0u)
        {
            task_priority_t priority =
                scheduler_task_effective_priority(task_index);

            if (selected == MUTEX_NO_OWNER || priority > selected_priority)
            {
                selected = task_index;
                selected_priority = priority;
            }
        }
    }

    if (selected != MUTEX_NO_OWNER)
    {
        mutex->waiters &= (uint8_t)~(1u << selected);
        mutex->owner = selected;
        scheduler_complete_wait(selected, WAIT_ACQUIRED);
    }

    return selected;
}

void mutex_init(mutex_t *mutex)
{
    uint8_t saved_sreg = SREG;
    uint8_t registered = 0u;

    cli();

    for (mutex_t *current = g_mutexes; current != 0; current = current->next)
    {
        if (current == mutex)
        {
            registered = 1u;
            break;
        }
    }

    mutex->waiters = 0u;
    mutex->owner = MUTEX_NO_OWNER;

    if (!registered)
    {
        mutex->next = g_mutexes;
        g_mutexes = mutex;
    }

    SREG = saved_sreg;
}

uint8_t mutex_lock(mutex_t *mutex)
{
    uint8_t saved_sreg = SREG;
    uint8_t current;

    cli();
    current = scheduler_current_task_index();

    if (mutex->owner == MUTEX_NO_OWNER)
    {
        mutex->owner = current;
        SREG = saved_sreg;
        return 1u;
    }

    /* This first version is deliberately non-recursive. */
    if (mutex->owner == current)
    {
        SREG = saved_sreg;
        return 0u;
    }

    mutex->waiters |= (uint8_t)(1u << current);
    scheduler_block_current();
    mutex_recompute_all_priorities();
    SREG = saved_sreg;

    task_yield();
    return 1u;
}

wait_result_t mutex_lock_timeout(mutex_t *mutex, uint32_t timeout_ms)
{
    uint8_t saved_sreg = SREG;
    uint8_t current;

    cli();
    current = scheduler_current_task_index();

    if (mutex->owner == MUTEX_NO_OWNER)
    {
        mutex->owner = current;
        SREG = saved_sreg;
        return WAIT_ACQUIRED;
    }

    if (mutex->owner == current)
    {
        SREG = saved_sreg;
        return WAIT_ERROR;
    }

    if (timeout_ms == 0u)
    {
        SREG = saved_sreg;
        return WAIT_TIMEOUT;
    }

    mutex->waiters |= (uint8_t)(1u << current);
    scheduler_block_current_timeout(timeout_ms, mutex, mutex_cancel_wait);
    mutex_recompute_all_priorities();
    SREG = saved_sreg;

    task_yield();
    return scheduler_current_wait_result();
}

uint8_t mutex_try_lock(mutex_t *mutex)
{
    uint8_t saved_sreg = SREG;
    uint8_t current;
    uint8_t locked = 0u;

    cli();
    current = scheduler_current_task_index();

    if (mutex->owner == MUTEX_NO_OWNER)
    {
        mutex->owner = current;
        locked = 1u;
    }

    SREG = saved_sreg;
    return locked;
}

uint8_t mutex_unlock(mutex_t *mutex)
{
    uint8_t saved_sreg = SREG;
    uint8_t current;
    uint8_t next_owner;

    cli();
    current = scheduler_current_task_index();

    if (mutex->owner != current)
    {
        SREG = saved_sreg;
        return 0u;
    }

    next_owner = mutex_handoff(mutex);
    if (next_owner == MUTEX_NO_OWNER)
    {
        mutex->owner = MUTEX_NO_OWNER;
    }

    mutex_recompute_all_priorities();
    SREG = saved_sreg;
    return 1u;
}
