#include "software_timer.h"

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

uint8_t SREG = 0x80u;

static uint32_t mock_now;

void host_cli(void)
{
    SREG &= (uint8_t)~0x80u;
}

uint32_t timer_millis(void)
{
    return mock_now;
}

void task_sleep(uint32_t delay_ms)
{
    (void)delay_ms;
}

static void dummy_callback(void *argument)
{
    (void)argument;
}

int main(void)
{
    software_timer_t once;
    software_timer_t periodic;
    software_timer_t wrapped;

    software_timer_system_init();
    software_timer_init(&once, dummy_callback, 0);
    software_timer_init(&periodic, dummy_callback, 0);
    software_timer_init(&wrapped, dummy_callback, 0);

    mock_now = 100u;
    assert(software_timer_start_once(&once, 10u));
    software_timer_process(109u);
    assert(!once.control.pending);
    software_timer_process(110u);
    assert(once.control.pending);
    assert(!once.control.active);

    mock_now = 200u;
    assert(software_timer_start_periodic(&periodic, 10u));
    software_timer_process(235u);
    assert(periodic.control.pending);
    assert(periodic.control.active);
    assert(periodic.deadline == 240u);

    periodic.control.pending = 0u; /* Simulate timer-service dispatch. */
    software_timer_process(240u);
    assert(periodic.control.pending);
    assert(periodic.deadline == 250u);
    software_timer_stop(&periodic);
    assert(!software_timer_is_active(&periodic));
    assert(!periodic.control.pending);

    mock_now = UINT32_MAX - 2u;
    assert(software_timer_start_once(&wrapped, 5u));
    assert(wrapped.deadline == 2u);
    software_timer_process(1u);
    assert(!wrapped.control.pending);
    software_timer_process(2u);
    assert(wrapped.control.pending);

    puts("software timer tests passed");
    return 0;
}
