#include "kernel/scheduler.h"
#include "kernel/software_timer.h"
#include "kernel/timer.h"
#include "drivers/uart.h"
#include "generated/log_messages.h"
#include "tasks/idle_task.h"
#include "tasks/led_task.h"
#include "tasks/queue_test_tasks.h"
#include "tasks/rv_demo_task.h"
#include "tasks/shell_task.h"

#include <avr/io.h>
#include <stdint.h>

#ifdef MESSAGE_QUEUE_SELF_TEST
#define TASK_COUNT 7u
#else
#define TASK_COUNT 5u
#endif

_Static_assert(LED_STACK_SIZE >
                       TASK_INITIAL_FRAME_SIZE + TASK_STACK_GUARD_SIZE &&
                   LED_STACK_SIZE <= UINT8_MAX,
               "LED stack cannot hold its initial context");
_Static_assert(SHELL_STACK_SIZE >
                       TASK_INITIAL_FRAME_SIZE + TASK_STACK_GUARD_SIZE &&
                   SHELL_STACK_SIZE <= UINT8_MAX,
               "shell stack cannot hold its initial context");
_Static_assert(TIMER_SERVICE_STACK_SIZE >
                       TASK_INITIAL_FRAME_SIZE + TASK_STACK_GUARD_SIZE &&
                   TIMER_SERVICE_STACK_SIZE <= UINT8_MAX,
               "timer service stack cannot hold its initial context");
_Static_assert(IDLE_STACK_SIZE >
                       TASK_INITIAL_FRAME_SIZE + TASK_STACK_GUARD_SIZE &&
                   IDLE_STACK_SIZE <= UINT8_MAX,
               "idle stack cannot hold its initial context");
_Static_assert(RV_DEMO_STACK_SIZE >
                       TASK_INITIAL_FRAME_SIZE + TASK_STACK_GUARD_SIZE &&
                   RV_DEMO_STACK_SIZE <= UINT8_MAX,
               "RV demo stack cannot hold its initial context");

static task_t tasks[TASK_COUNT];
static uint8_t led_stack[LED_STACK_SIZE];
static uint8_t shell_stack[SHELL_STACK_SIZE];
static uint8_t timer_service_stack[TIMER_SERVICE_STACK_SIZE];
static uint8_t idle_stack[IDLE_STACK_SIZE];
static uint8_t rv_demo_stack[RV_DEMO_STACK_SIZE];
#ifdef MESSAGE_QUEUE_SELF_TEST
static uint8_t queue_producer_stack[96u];
static uint8_t queue_consumer_stack[112u];
#endif

int main(void)
{
    /* A software reboot does not reset peripheral registers.  Start from a
     * known state so stale pin-change enables cannot invoke an unowned ISR. */
    PCICR = 0u;
    PCMSK0 = 0u;
    PCMSK1 = 0u;
    PCMSK2 = 0u;
#ifdef PCMSK3
    PCMSK3 = 0u;
#endif

    DDRB |= (1u << DDB5);

    timer_init();
    software_timer_system_init();
    uart_init();
    led_task_init();

    uart_write_compressed_P(log_boot_data, LOG_BOOT_BYTE_COUNT,
                            LOG_BOOT_BIT_COUNT);

#ifdef MESSAGE_QUEUE_SELF_TEST
    queue_test_init();
#endif

    task_init(&tasks[0], led_task, led_stack, sizeof(led_stack),
              TASK_PRIORITY_LOW);
    task_init(&tasks[1], shell_task, shell_stack,
              sizeof(shell_stack), TASK_PRIORITY_HIGH);
    task_init(&tasks[2], software_timer_task, timer_service_stack,
              sizeof(timer_service_stack), TASK_PRIORITY_NORMAL);
    task_init(&tasks[3], idle_task, idle_stack, sizeof(idle_stack),
              TASK_PRIORITY_IDLE);
    task_init(&tasks[4], rv_demo_task, rv_demo_stack,
              sizeof(rv_demo_stack), TASK_PRIORITY_NORMAL);
#ifdef MESSAGE_QUEUE_SELF_TEST
    task_init(&tasks[5], queue_test_producer, queue_producer_stack,
              sizeof(queue_producer_stack), TASK_PRIORITY_NORMAL);
    task_init(&tasks[6], queue_test_consumer, queue_consumer_stack,
              sizeof(queue_consumer_stack), TASK_PRIORITY_HIGH);
#endif

    scheduler_init(tasks, TASK_COUNT);
    /* timer_init() runs before the UART boot message and task construction.
     * Drop the compare flag accumulated during that startup work so the first
     * tick cannot preempt the first task immediately after scheduler_start. */
    TIFR2 = (1u << OCF2A);
    scheduler_start();
}
