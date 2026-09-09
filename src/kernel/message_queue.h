#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include "mutex.h"
#include "scheduler.h"
#include "semaphore.h"

#include <stdint.h>

typedef struct
{
    uint8_t *storage;
    uint8_t item_size;
    uint8_t capacity;
    uint8_t head;
    uint8_t tail;
    mutex_t lock;
    semaphore_t items;
    semaphore_t spaces;
} message_queue_t;

uint8_t message_queue_init(message_queue_t *queue, void *storage,
                           uint8_t capacity, uint8_t item_size);
wait_result_t message_queue_send(message_queue_t *queue, const void *item);
wait_result_t message_queue_receive(message_queue_t *queue, void *item);
wait_result_t message_queue_send_timeout(message_queue_t *queue,
                                         const void *item,
                                         uint32_t timeout_ms);
wait_result_t message_queue_receive_timeout(message_queue_t *queue, void *item,
                                            uint32_t timeout_ms);
uint8_t message_queue_try_send(message_queue_t *queue, const void *item);
uint8_t message_queue_try_receive(message_queue_t *queue, void *item);
uint8_t message_queue_count(message_queue_t *queue);

#endif
