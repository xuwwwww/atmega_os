#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "scheduler.h"

#include <stdint.h>

typedef struct
{
    volatile uint8_t waiters;
    volatile uint8_t count;
    uint8_t max_count;
} semaphore_t;

void semaphore_init(semaphore_t *semaphore, uint8_t initial_count,
                    uint8_t max_count);
void semaphore_take(semaphore_t *semaphore);
wait_result_t semaphore_take_timeout(semaphore_t *semaphore,
                                     uint32_t timeout_ms);
uint8_t semaphore_try_take(semaphore_t *semaphore);
uint8_t semaphore_give(semaphore_t *semaphore);
void semaphore_give_from_isr(semaphore_t *semaphore);

#endif
