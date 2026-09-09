#include "scheduler.h"

#include "panic.h"
#include "timer.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

#define STACK_FILL_BYTE 0xa5u
#define STACK_GUARD_BYTE 0xd3u
#define INITIAL_SREG (1u << SREG_I)

_Static_assert(sizeof(task_t) == 16u,
               "task_t layout changed; verify context.S and SRAM budget");

/* context.S accesses this symbol directly. */
task_t* volatile g_current_task = 0;
static task_t *g_tasks = 0;
static uint8_t g_task_count = 0;
static uint8_t g_current_index = 0;
static volatile uint8_t g_preempt_depth = 0;

static void scheduler_select_next_at(uint32_t now);

void task_init(task_t *task, task_function_t entry, uint8_t *stack,
               uint8_t stack_size, task_priority_t priority)
{
    uint8_t *sp = stack + stack_size - 1u;
    uint16_t entry_address = (uint16_t)entry;
    uint16_t exit_address = (uint16_t)task_exit;

    for (uint16_t i = 0; i < stack_size; i++)
    {
        stack[i] = STACK_FILL_BYTE;
    }

    for (uint8_t i = 0u; i < TASK_STACK_GUARD_SIZE; i++)
    {
        stack[i] = STACK_GUARD_BYTE;
    }

    /* Make a returning task enter task_exit(), exactly as though a normal
     * CALL had placed task_exit's address above the task's initial frame. */
    *sp-- = (uint8_t)exit_address;
    *sp-- = (uint8_t)(exit_address >> 8);

    /* Fake the two-byte PC that scheduler_start's RET loads first. */
    *sp-- = (uint8_t)entry_address;
    *sp-- = (uint8_t)(entry_address >> 8);

    /* Fake the frame expected by context_restore: r0, SREG, r1, r2...r31. */
    *sp-- = 0u;
    *sp-- = INITIAL_SREG;
    *sp-- = 0u;

    for (uint8_t reg = 2u; reg <= 31u; reg++)
    {
        *sp-- = 0u;
    }

    task->saved_sp = sp;
    task->stack = stack;
    task->wake_time = 0;
    task->entry = entry;
    task->wait_object = 0;
    task->wait_cancel = 0;
    task->stack_size = stack_size;
    task->control.state = TASK_READY;
    if (priority > TASK_PRIORITY_HIGH)
    {
        priority = TASK_PRIORITY_HIGH;
    }
    task->control.base_priority = priority;
    task->control.effective_priority = priority;
    task->control.timed_wait = 0;
}

uint8_t task_stack_unused(const task_t *task)
{
    uint8_t index = TASK_STACK_GUARD_SIZE;

    while (index < task->stack_size &&
           task->stack[index] == STACK_FILL_BYTE)
    {
        index++;
    }

    return (uint8_t)(index - TASK_STACK_GUARD_SIZE);
}

uint8_t task_stack_guard_ok(const task_t *task)
{
    for (uint8_t i = 0u; i < TASK_STACK_GUARD_SIZE; i++)
    {
        if (task->stack[i] != STACK_GUARD_BYTE)
        {
            return 0u;
        }
    }

    return 1u;
}

uint8_t scheduler_task_count(void)
{
    return g_task_count;
}

uint8_t scheduler_task_stack_unused(uint8_t task_index)
{
    if (task_index >= g_task_count)
    {
        return 0;
    }

    return task_stack_unused(&g_tasks[task_index]);
}

uint8_t scheduler_task_stack_size(uint8_t task_index)
{
    if (task_index >= g_task_count)
    {
        return 0;
    }

    return g_tasks[task_index].stack_size;
}

task_state_t scheduler_task_state(uint8_t task_index)
{
    uint8_t saved_sreg = SREG;
    task_state_t state = TASK_TERMINATED;

    cli();
    if (task_index < g_task_count)
    {
        state = g_tasks[task_index].control.state;
    }
    SREG = saved_sreg;

    return state;
}

task_priority_t scheduler_task_base_priority(uint8_t task_index)
{
    uint8_t saved_sreg = SREG;
    task_priority_t priority = TASK_PRIORITY_IDLE;

    cli();
    if (task_index < g_task_count)
    {
        priority = g_tasks[task_index].control.base_priority;
    }
    SREG = saved_sreg;

    return priority;
}

task_priority_t scheduler_task_effective_priority(uint8_t task_index)
{
    uint8_t saved_sreg = SREG;
    task_priority_t priority = TASK_PRIORITY_IDLE;

    cli();
    if (task_index < g_task_count)
    {
        priority = g_tasks[task_index].control.effective_priority;
    }
    SREG = saved_sreg;

    return priority;
}

uint8_t task_restart(uint8_t task_index)
{
    uint8_t saved_sreg = SREG;
    task_function_t entry;
    uint8_t *stack;
    uint8_t stack_size;
    task_priority_t priority;

    cli();

    if (task_index >= g_task_count ||
        g_tasks[task_index].control.state != TASK_TERMINATED)
    {
        SREG = saved_sreg;
        return 0;
    }

    entry = g_tasks[task_index].entry;
    stack = g_tasks[task_index].stack;
    stack_size = g_tasks[task_index].stack_size;
    priority = g_tasks[task_index].control.base_priority;
    task_init(&g_tasks[task_index], entry, stack, stack_size, priority);

    SREG = saved_sreg;
    return 1;
}

void scheduler_init(task_t *tasks, uint8_t task_count)
{
    if (task_count == 0u || task_count > SCHEDULER_MAX_TASKS)
    {
        kernel_panic(PANIC_INVALID_SCHEDULER, task_count);
    }

    g_tasks = tasks;
    g_task_count = task_count;
    g_current_index = 0;

    for (uint8_t i = 1u; i < task_count; i++)
    {
        if (tasks[i].control.effective_priority >
            tasks[g_current_index].control.effective_priority)
        {
            g_current_index = i;
        }
    }

    g_current_task = &tasks[g_current_index];
    g_current_task->control.state = TASK_RUNNING;
}

void task_sleep(uint32_t delay_ms)
{
    uint8_t saved_sreg;

    if (delay_ms == 0u)
    {
        task_yield();
        return;
    }

    saved_sreg = SREG;
    cli();
    g_current_task->wake_time = timer_millis() + delay_ms;
    g_current_task->wait_object = 0;
    g_current_task->wait_cancel = 0;
    g_current_task->control.timed_wait = 0;
    g_current_task->control.state = TASK_SLEEPING;
    SREG = saved_sreg;

    task_yield();
}

void task_exit(void)
{
    cli();
    g_current_task->control.state = TASK_TERMINATED;
    task_yield();

    __builtin_unreachable();
}

void scheduler_preempt_disable(void)
{
    uint8_t saved_sreg = SREG;

    cli();
    g_preempt_depth++;
    SREG = saved_sreg;
}

void scheduler_preempt_enable(void)
{
    uint8_t saved_sreg = SREG;

    cli();
    if (g_preempt_depth > 0u)
    {
        g_preempt_depth--;
    }
    SREG = saved_sreg;
}

uint8_t scheduler_current_task_index(void)
{
    return g_current_index;
}

void scheduler_block_current(void)
{
    g_current_task->wake_time = WAIT_ACQUIRED;
    g_current_task->wait_object = 0;
    g_current_task->wait_cancel = 0;
    g_current_task->control.timed_wait = 0;
    g_current_task->control.state = TASK_BLOCKED;
}

void scheduler_block_current_timeout(uint32_t timeout_ms, void *object,
                                     task_wait_cancel_t cancel)
{
    g_current_task->wake_time = timer_millis() + timeout_ms;
    g_current_task->wait_object = object;
    g_current_task->wait_cancel = cancel;
    g_current_task->control.timed_wait = 1u;
    g_current_task->control.state = TASK_BLOCKED;
}

void scheduler_make_ready(uint8_t task_index)
{
    if (task_index < g_task_count &&
        g_tasks[task_index].control.state == TASK_BLOCKED)
    {
        g_tasks[task_index].control.state = TASK_READY;
    }
}

void scheduler_complete_wait(uint8_t task_index, wait_result_t result)
{
    scheduler_complete_wait_value(task_index, result, 0u);
}

void scheduler_complete_wait_value(uint8_t task_index, wait_result_t result,
                                   uint8_t value)
{
    if (task_index < g_task_count &&
        g_tasks[task_index].control.state == TASK_BLOCKED)
    {
        /* A completed wait no longer needs its deadline. Reuse the same four
         * bytes for the result and a one-byte synchronization payload. */
        g_tasks[task_index].wake_time =
            (uint32_t)result | ((uint32_t)value << 8u);
        g_tasks[task_index].wait_object = 0;
        g_tasks[task_index].wait_cancel = 0;
        g_tasks[task_index].control.timed_wait = 0;
        g_tasks[task_index].control.state = TASK_READY;
    }
}

wait_result_t scheduler_current_wait_result(void)
{
    return (wait_result_t)(g_current_task->wake_time & 0xffu);
}

uint8_t scheduler_current_wait_value(void)
{
    return (uint8_t)((g_current_task->wake_time >> 8u) & 0xffu);
}

void scheduler_set_effective_priority(uint8_t task_index,
                                      task_priority_t priority)
{
    if (task_index >= g_task_count)
    {
        return;
    }

    if (priority > TASK_PRIORITY_HIGH)
    {
        priority = TASK_PRIORITY_HIGH;
    }

    g_tasks[task_index].control.effective_priority = priority;
}

static void scheduler_select_next_at(uint32_t now)
{
    uint8_t candidate_index = g_current_index;
    uint8_t selected_index = g_current_index;
    uint8_t found = 0u;
    task_priority_t best_priority = TASK_PRIORITY_IDLE;

    for (uint8_t i = 0u; i < g_task_count; i++)
    {
        if (!task_stack_guard_ok(&g_tasks[i]))
        {
            kernel_panic(PANIC_STACK_OVERFLOW, i);
        }
    }

    if (g_current_task->control.state == TASK_RUNNING)
    {
        g_current_task->control.state = TASK_READY;
    }

    for (uint8_t i = 0; i < g_task_count; i++)
    {
        task_t *task = &g_tasks[i];

        if (task->control.state == TASK_SLEEPING &&
            (int32_t)(now - task->wake_time) >= 0)
        {
            task->control.state = TASK_READY;
        }

        if (task->control.state == TASK_BLOCKED &&
            task->control.timed_wait &&
            (int32_t)(now - task->wake_time) >= 0)
        {
            task_wait_cancel_t cancel = task->wait_cancel;
            void *object = task->wait_object;

            task->control.timed_wait = 0;
            task->wait_cancel = 0;
            task->wait_object = 0;

            if (cancel != 0)
            {
                cancel(object, i);
            }

            task->wake_time = WAIT_TIMEOUT;
            task->control.state = TASK_READY;
        }
    }

    for (uint8_t scanned = 0; scanned < g_task_count; scanned++)
    {
        candidate_index++;

        if (candidate_index >= g_task_count)
        {
            candidate_index = 0;
        }

        task_t *candidate = &g_tasks[candidate_index];

        if (candidate->control.state == TASK_READY &&
            (!found || candidate->control.effective_priority > best_priority))
        {
            selected_index = candidate_index;
            best_priority = candidate->control.effective_priority;
            found = 1u;
        }
    }

    if (found)
    {
        g_current_index = selected_index;
        g_current_task = &g_tasks[selected_index];
        g_current_task->control.state = TASK_RUNNING;
        return;
    }

    /* An idle task should make this unreachable. Keep the kernel recoverable. */
    g_current_task->control.state = TASK_RUNNING;
}

/* Called from context.S after a cooperative context save. */
void scheduler_select_next(void)
{
    scheduler_select_next_at(timer_millis());
    /* context.S calls this on the kernel stack and must regain control before
     * it changes SP to the selected task's private stack. */
    __asm__ __volatile__("" ::: "memory");
}

/* Called from the naked Timer2 ISR after the interrupted context is saved. */
void scheduler_tick(void)
{
    uint32_t now = timer_tick_from_isr();

    if (g_preempt_depth == 0u)
    {
        scheduler_select_next_at(now);
        /* See scheduler_select_next(): context.S changes SP only after this
         * function has returned on the kernel stack. */
        __asm__ __volatile__("" ::: "memory");
    }
}
