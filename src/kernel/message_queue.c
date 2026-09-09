#include "message_queue.h"

#include <stddef.h>

#ifdef __AVR__
_Static_assert(sizeof(message_queue_t) == 16u,
               "message_queue_t layout changed; verify SRAM budget");
#endif

static uint8_t message_queue_valid(const message_queue_t *queue,
                                   const void *item)
{
    return queue != NULL && queue->storage != NULL && item != NULL &&
           queue->capacity != 0u && queue->item_size != 0u;
}

static void message_queue_copy_in(message_queue_t *queue, const void *item)
{
    const uint8_t *source = item;
    uint16_t offset = (uint16_t)queue->head * queue->item_size;

    for (uint8_t i = 0; i < queue->item_size; i++)
    {
        queue->storage[offset + i] = source[i];
    }

    queue->head++;
    if (queue->head >= queue->capacity)
    {
        queue->head = 0u;
    }
}

static void message_queue_copy_out(message_queue_t *queue, void *item)
{
    uint8_t *destination = item;
    uint16_t offset = (uint16_t)queue->tail * queue->item_size;

    for (uint8_t i = 0; i < queue->item_size; i++)
    {
        destination[i] = queue->storage[offset + i];
    }

    queue->tail++;
    if (queue->tail >= queue->capacity)
    {
        queue->tail = 0u;
    }
}

uint8_t message_queue_init(message_queue_t *queue, void *storage,
                           uint8_t capacity, uint8_t item_size)
{
    if (queue == NULL || storage == NULL || capacity == 0u || item_size == 0u)
    {
        return 0u;
    }

    queue->storage = storage;
    queue->item_size = item_size;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->tail = 0u;
    mutex_init(&queue->lock);
    semaphore_init(&queue->items, 0u, capacity);
    semaphore_init(&queue->spaces, capacity, capacity);
    return 1u;
}

wait_result_t message_queue_send(message_queue_t *queue, const void *item)
{
    if (!message_queue_valid(queue, item))
    {
        return WAIT_ERROR;
    }

    semaphore_take(&queue->spaces);
    if (!mutex_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->spaces);
        return WAIT_ERROR;
    }

    message_queue_copy_in(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->items);
    return WAIT_ACQUIRED;
}

wait_result_t message_queue_receive(message_queue_t *queue, void *item)
{
    if (!message_queue_valid(queue, item))
    {
        return WAIT_ERROR;
    }

    semaphore_take(&queue->items);
    if (!mutex_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->items);
        return WAIT_ERROR;
    }

    message_queue_copy_out(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->spaces);
    return WAIT_ACQUIRED;
}

wait_result_t message_queue_send_timeout(message_queue_t *queue,
                                         const void *item,
                                         uint32_t timeout_ms)
{
    wait_result_t result;

    if (!message_queue_valid(queue, item))
    {
        return WAIT_ERROR;
    }

    result = semaphore_take_timeout(&queue->spaces, timeout_ms);
    if (result != WAIT_ACQUIRED)
    {
        return result;
    }

    if (!mutex_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->spaces);
        return WAIT_ERROR;
    }

    message_queue_copy_in(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->items);
    return WAIT_ACQUIRED;
}

wait_result_t message_queue_receive_timeout(message_queue_t *queue, void *item,
                                            uint32_t timeout_ms)
{
    wait_result_t result;

    if (!message_queue_valid(queue, item))
    {
        return WAIT_ERROR;
    }

    result = semaphore_take_timeout(&queue->items, timeout_ms);
    if (result != WAIT_ACQUIRED)
    {
        return result;
    }

    if (!mutex_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->items);
        return WAIT_ERROR;
    }

    message_queue_copy_out(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->spaces);
    return WAIT_ACQUIRED;
}

uint8_t message_queue_try_send(message_queue_t *queue, const void *item)
{
    if (!message_queue_valid(queue, item) ||
        !semaphore_try_take(&queue->spaces))
    {
        return 0u;
    }

    if (!mutex_try_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->spaces);
        return 0u;
    }

    message_queue_copy_in(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->items);
    return 1u;
}

uint8_t message_queue_try_receive(message_queue_t *queue, void *item)
{
    if (!message_queue_valid(queue, item) ||
        !semaphore_try_take(&queue->items))
    {
        return 0u;
    }

    if (!mutex_try_lock(&queue->lock))
    {
        (void)semaphore_give(&queue->items);
        return 0u;
    }

    message_queue_copy_out(queue, item);
    (void)mutex_unlock(&queue->lock);
    (void)semaphore_give(&queue->spaces);
    return 1u;
}

uint8_t message_queue_count(message_queue_t *queue)
{
    uint8_t count;

    if (queue == NULL)
    {
        return 0u;
    }

    count = queue->items.count;
    return count;
}
