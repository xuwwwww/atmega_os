#include "led_task.h"

#include "../kernel/event_flags.h"
#include "../kernel/scheduler.h"
#include "../kernel/software_timer.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

#define LED_EVENT_MODE_CHANGED (1u << 0)
#define LED_EVENT_TIMER_EXPIRED (1u << 1)

static event_flags_t led_events;
static software_timer_t led_timer;
static volatile uint8_t led_mode = LED_MODE_BLINK;
static volatile uint8_t led_on = 0u;

static void led_timer_expired(void *argument)
{
    (void)argument;
    event_flags_set(&led_events, LED_EVENT_TIMER_EXPIRED);
}

void led_task_init(void)
{
    event_flags_init(&led_events, 0u);
    software_timer_init(&led_timer, led_timer_expired, 0);
    led_mode = LED_MODE_BLINK;
    led_on = 0u;
    PORTB &= (uint8_t)~(1u << PORTB5);
    (void)software_timer_start_once(&led_timer, 2000u);
}

void led_task_set_mode(uint8_t mode)
{
    uint8_t saved_sreg;

    if (mode > LED_MODE_ON)
    {
        return;
    }

    saved_sreg = SREG;
    cli();
    led_mode = mode;

    if (mode == LED_MODE_ON)
    {
        led_on = 1u;
        PORTB |= (1u << PORTB5);
    }
    else if (mode == LED_MODE_OFF)
    {
        led_on = 0u;
        PORTB &= (uint8_t)~(1u << PORTB5);
    }
    SREG = saved_sreg;

    /* Wake the LED task immediately if it is waiting for its next blink. */
    event_flags_set(&led_events, LED_EVENT_MODE_CHANGED);
}

uint8_t led_task_get_mode(void)
{
    uint8_t saved_sreg = SREG;
    uint8_t mode;

    cli();
    mode = led_mode;
    SREG = saved_sreg;
    return mode;
}

void led_task(void)
{
    for (;;)
    {
        uint8_t events;

        (void)event_flags_wait(
            &led_events,
            LED_EVENT_MODE_CHANGED | LED_EVENT_TIMER_EXPIRED,
            EVENT_WAIT_ANY | EVENT_AUTO_CLEAR, &events);

        if (led_task_get_mode() != LED_MODE_BLINK)
        {
            software_timer_stop(&led_timer);
        }
        else
        {
            /* A mode command wins over an old timer event. Starting blink
             * always begins a fresh interval from the current LED state. */
            if ((events & LED_EVENT_TIMER_EXPIRED) != 0u &&
                (events & LED_EVENT_MODE_CHANGED) == 0u)
            {
                led_on = !led_on;
                PORTB ^= (1u << PORTB5);
            }

            (void)software_timer_start_once(&led_timer,
                                             led_on ? 100u : 2000u);
        }
    }
}
