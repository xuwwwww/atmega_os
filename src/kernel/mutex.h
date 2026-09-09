#ifndef MUTEX_H
#define MUTEX_H

#include "scheduler.h"

#include <stdint.h>

#define MUTEX_NO_OWNER 0xffu

typedef struct mutex
{
    struct mutex *next;
    volatile uint8_t waiters;
    volatile uint8_t owner;
} mutex_t;

void mutex_init(mutex_t *mutex);
uint8_t mutex_lock(mutex_t *mutex);
wait_result_t mutex_lock_timeout(mutex_t *mutex, uint32_t timeout_ms);
uint8_t mutex_try_lock(mutex_t *mutex);
uint8_t mutex_unlock(mutex_t *mutex);

#endif
