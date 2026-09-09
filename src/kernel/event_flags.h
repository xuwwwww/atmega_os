#ifndef EVENT_FLAGS_H
#define EVENT_FLAGS_H

#include "scheduler.h"

#include <stdint.h>

#define EVENT_WAIT_ANY 0x00u
#define EVENT_WAIT_ALL 0x01u
#define EVENT_AUTO_CLEAR 0x02u

typedef struct
{
    volatile uint8_t bits;
    volatile uint8_t waiters;
    uint8_t wait_all;
    uint8_t auto_clear;
    uint8_t masks[SCHEDULER_MAX_TASKS];
} event_flags_t;

void event_flags_init(event_flags_t *events, uint8_t initial_bits);
uint8_t event_flags_get(event_flags_t *events);
void event_flags_clear(event_flags_t *events, uint8_t mask);
void event_flags_set(event_flags_t *events, uint8_t mask);
void event_flags_set_from_isr(event_flags_t *events, uint8_t mask);
uint8_t event_flags_try_wait(event_flags_t *events, uint8_t mask,
                             uint8_t options, uint8_t *matched_bits);
wait_result_t event_flags_wait(event_flags_t *events, uint8_t mask,
                               uint8_t options, uint8_t *matched_bits);
wait_result_t event_flags_wait_timeout(event_flags_t *events, uint8_t mask,
                                       uint8_t options, uint32_t timeout_ms,
                                       uint8_t *matched_bits);

#endif
