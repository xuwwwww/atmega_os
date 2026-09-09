#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

#define TASK_INITIAL_FRAME_SIZE 37u
#define TASK_STACK_GUARD_SIZE 4u
#define SCHEDULER_MAX_TASKS 8u

typedef void (*task_function_t)(void);

typedef uint8_t task_state_t;
typedef uint8_t task_priority_t;
typedef uint8_t wait_result_t;
typedef void (*task_wait_cancel_t)(void *object, uint8_t task_index);

enum
{
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_TERMINATED,
    TASK_BLOCKED,
};

enum
{
    WAIT_ACQUIRED = 0,
    WAIT_TIMEOUT,
    WAIT_ERROR,
};

enum
{
    TASK_PRIORITY_IDLE = 0,
    TASK_PRIORITY_LOW,
    TASK_PRIORITY_NORMAL,
    TASK_PRIORITY_HIGH,
};

typedef struct
{
    /* Must remain first: context.S stores the saved SP at offset zero. */
    uint8_t *saved_sp;
    uint8_t *stack;
    uint32_t wake_time;
    task_function_t entry;
    void *wait_object;
    task_wait_cancel_t wait_cancel;
    uint8_t stack_size;
    struct
    {
        uint8_t state : 3;
        uint8_t base_priority : 2;
        uint8_t effective_priority : 2;
        uint8_t timed_wait : 1;
    } control;
} task_t;

void task_init(task_t *task, task_function_t entry, uint8_t *stack,
               uint8_t stack_size, task_priority_t priority);
uint8_t task_stack_unused(const task_t *task);
uint8_t task_stack_guard_ok(const task_t *task);
uint8_t scheduler_task_count(void);
uint8_t scheduler_task_stack_unused(uint8_t task_index);
uint8_t scheduler_task_stack_size(uint8_t task_index);
task_state_t scheduler_task_state(uint8_t task_index);
task_priority_t scheduler_task_base_priority(uint8_t task_index);
task_priority_t scheduler_task_effective_priority(uint8_t task_index);
uint8_t task_restart(uint8_t task_index);

void scheduler_init(task_t *tasks, uint8_t task_count);
void scheduler_start(void) __attribute__((noreturn));
void task_yield(void);
void task_sleep(uint32_t delay_ms);
void task_exit(void) __attribute__((noreturn));
void scheduler_preempt_disable(void);
void scheduler_preempt_enable(void);

/* Kernel synchronization hooks. Call only with interrupts disabled. */
uint8_t scheduler_current_task_index(void);
void scheduler_block_current(void);
void scheduler_block_current_timeout(uint32_t timeout_ms, void *object,
                                     task_wait_cancel_t cancel);
void scheduler_make_ready(uint8_t task_index);
void scheduler_complete_wait(uint8_t task_index, wait_result_t result);
void scheduler_complete_wait_value(uint8_t task_index, wait_result_t result,
                                   uint8_t value);
wait_result_t scheduler_current_wait_result(void);
uint8_t scheduler_current_wait_value(void);
void scheduler_set_effective_priority(uint8_t task_index,
                                      task_priority_t priority);

#endif
