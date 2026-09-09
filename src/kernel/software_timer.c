#include "software_timer.h"

#include "scheduler.h"
#include "timer.h"

#include <avr/interrupt.h>
#include <avr/io.h>

#ifdef __AVR__
_Static_assert(sizeof(software_timer_t) == 15u,
               "software_timer_t layout changed; verify SRAM budget");
#endif

static software_timer_t *g_timers = 0;

void software_timer_system_init(void)
{
    g_timers = 0;
}

void software_timer_init(software_timer_t *timer,
                         software_timer_callback_t callback,
                         void *argument)
{
    uint8_t saved_sreg = SREG;

    cli();

    /* Reinitializing an already registered timer must not link it twice. */
    for (software_timer_t *current = g_timers;
         current != 0; current = current->next)
    {
        if (current == timer)
        {
            timer->callback = callback;
            timer->argument = argument;
            timer->deadline = 0u;
            timer->period = 0u;
            timer->control.active = 0u;
            timer->control.periodic = 0u;
            timer->control.pending = 0u;
            timer->control.reserved = 0u;
            SREG = saved_sreg;
            return;
        }
    }

    timer->next = g_timers;
    timer->callback = callback;
    timer->argument = argument;
    timer->deadline = 0u;
    timer->period = 0u;
    timer->control.active = 0u;
    timer->control.periodic = 0u;
    timer->control.pending = 0u;
    timer->control.reserved = 0u;
    g_timers = timer;

    SREG = saved_sreg;
}

uint8_t software_timer_start_once(software_timer_t *timer,
                                  uint32_t delay_ms)
{
    uint8_t saved_sreg;

    if (timer == 0 || timer->callback == 0)
    {
        return 0u;
    }

    saved_sreg = SREG;
    cli();
    timer->deadline = timer_millis() + delay_ms;
    timer->period = 0u;
    timer->control.periodic = 0u;
    timer->control.pending = 0u;
    timer->control.active = 1u;
    SREG = saved_sreg;
    return 1u;
}

uint8_t software_timer_start_periodic(software_timer_t *timer,
                                      uint32_t period_ms)
{
    uint8_t saved_sreg;

    if (timer == 0 || timer->callback == 0 || period_ms == 0u)
    {
        return 0u;
    }

    saved_sreg = SREG;
    cli();
    timer->deadline = timer_millis() + period_ms;
    timer->period = period_ms;
    timer->control.periodic = 1u;
    timer->control.pending = 0u;
    timer->control.active = 1u;
    SREG = saved_sreg;
    return 1u;
}

void software_timer_stop(software_timer_t *timer)
{
    uint8_t saved_sreg;

    if (timer == 0)
    {
        return;
    }

    saved_sreg = SREG;
    cli();
    timer->control.active = 0u;
    timer->control.pending = 0u;
    SREG = saved_sreg;
}

uint8_t software_timer_is_active(software_timer_t *timer)
{
    uint8_t saved_sreg;
    uint8_t active;

    if (timer == 0)
    {
        return 0u;
    }

    saved_sreg = SREG;
    cli();
    active = timer->control.active;
    SREG = saved_sreg;
    return active;
}

void software_timer_process(uint32_t now)
{
    for (software_timer_t *timer = g_timers;
         timer != 0; timer = timer->next)
    {
        if (!timer->control.active || timer->control.pending ||
            (int32_t)(now - timer->deadline) < 0)
        {
            continue;
        }

        timer->control.pending = 1u;

        if (timer->control.periodic)
        {
            do
            {
                timer->deadline += timer->period;
            } while ((int32_t)(now - timer->deadline) >= 0);
        }
        else
        {
            timer->control.active = 0u;
        }
    }

}

static software_timer_t *software_timer_take_pending(void)
{
    uint8_t saved_sreg = SREG;
    software_timer_t *selected = 0;

    cli();
    for (software_timer_t *timer = g_timers;
         timer != 0; timer = timer->next)
    {
        if (timer->control.pending)
        {
            timer->control.pending = 0u;
            selected = timer;
            break;
        }
    }
    SREG = saved_sreg;
    return selected;
}

void software_timer_task(void)
{
    for (;;)
    {
        software_timer_t *timer;

        task_sleep(1u);
        software_timer_process(timer_millis());

        while ((timer = software_timer_take_pending()) != 0)
        {
            timer->callback(timer->argument);
        }
    }
}
