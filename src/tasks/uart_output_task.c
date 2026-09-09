#include "uart_output_task.h"

#include "../drivers/uart.h"
#include "../kernel/scheduler.h"

#define STACK_REPORT_PERIODS 10u
#define STACK_REPORT_TASKS 4u

/* Legacy telemetry example. The normal Makefile no longer builds or registers
 * this task; shell commands now provide telemetry on demand. */

void uart_output_task(void)
{
    uint8_t periods = 0;

    for (;;)
    {
        task_sleep(1000u);

        periods++;
        if (periods >= STACK_REPORT_PERIODS)
        {
            uint8_t report[STACK_REPORT_TASKS * 4u];

            periods = 0;
            for (uint8_t i = 0; i < STACK_REPORT_TASKS; i++)
            {
                report[i] = scheduler_task_stack_unused(i);
                report[STACK_REPORT_TASKS + i] = scheduler_task_state(i);
                report[(STACK_REPORT_TASKS * 2u) + i] =
                    scheduler_task_base_priority(i);
                report[(STACK_REPORT_TASKS * 3u) + i] =
                    scheduler_task_effective_priority(i);
            }

            uart_write_binary_frame(UART_FRAME_STACK_REPORT, report,
                                    sizeof(report));
        }
    }
}
