#ifndef SOFTWARE_TIMER_H
#define SOFTWARE_TIMER_H

#include <stdint.h>

typedef void (*software_timer_callback_t)(void *argument);

typedef struct software_timer
{
    struct software_timer *next;
    software_timer_callback_t callback;
    void *argument;
    uint32_t deadline;
    uint32_t period;
    struct
    {
        uint8_t active : 1;
        uint8_t periodic : 1;
        uint8_t pending : 1;
        uint8_t reserved : 5;
    } control;
} software_timer_t;

void software_timer_system_init(void);
void software_timer_init(software_timer_t *timer,
                         software_timer_callback_t callback,
                         void *argument);
uint8_t software_timer_start_once(software_timer_t *timer,
                                  uint32_t delay_ms);
uint8_t software_timer_start_periodic(software_timer_t *timer,
                                      uint32_t period_ms);
void software_timer_stop(software_timer_t *timer);
uint8_t software_timer_is_active(software_timer_t *timer);

/* Called by the timer-service task; callbacks never execute in an ISR. */
void software_timer_process(uint32_t now);
void software_timer_task(void);

#endif
