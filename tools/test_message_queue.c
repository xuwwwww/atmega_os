#include "message_queue.h"

#include <assert.h>
#include <stdio.h>

void mutex_init(mutex_t *mutex)
{
    mutex->next = NULL;
    mutex->waiters = 0u;
    mutex->owner = MUTEX_NO_OWNER;
}

uint8_t mutex_lock(mutex_t *mutex)
{
    if (mutex->owner != MUTEX_NO_OWNER)
    {
        return 0u;
    }
    mutex->owner = 0u;
    return 1u;
}

uint8_t mutex_try_lock(mutex_t *mutex)
{
    return mutex_lock(mutex);
}

uint8_t mutex_unlock(mutex_t *mutex)
{
    mutex->owner = MUTEX_NO_OWNER;
    return 1u;
}

void semaphore_init(semaphore_t *semaphore, uint8_t initial_count,
                    uint8_t max_count)
{
    semaphore->waiters = 0u;
    semaphore->count = initial_count;
    semaphore->max_count = max_count;
}

void semaphore_take(semaphore_t *semaphore)
{
    assert(semaphore->count > 0u);
    semaphore->count--;
}

wait_result_t semaphore_take_timeout(semaphore_t *semaphore,
                                     uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (semaphore->count == 0u)
    {
        return WAIT_TIMEOUT;
    }
    semaphore->count--;
    return WAIT_ACQUIRED;
}

uint8_t semaphore_try_take(semaphore_t *semaphore)
{
    if (semaphore->count == 0u)
    {
        return 0u;
    }
    semaphore->count--;
    return 1u;
}

uint8_t semaphore_give(semaphore_t *semaphore)
{
    if (semaphore->count >= semaphore->max_count)
    {
        return 0u;
    }
    semaphore->count++;
    return 1u;
}

static void test_fifo_and_wraparound(void)
{
    message_queue_t queue;
    uint16_t storage[3];
    uint16_t input;
    uint16_t output = 0u;

    assert(message_queue_init(&queue, storage, 3u, sizeof(uint16_t)));
    assert(!message_queue_try_receive(&queue, &output));

    input = 11u;
    assert(message_queue_try_send(&queue, &input));
    input = 22u;
    assert(message_queue_try_send(&queue, &input));
    input = 33u;
    assert(message_queue_try_send(&queue, &input));
    input = 44u;
    assert(!message_queue_try_send(&queue, &input));
    assert(message_queue_count(&queue) == 3u);

    assert(message_queue_try_receive(&queue, &output) && output == 11u);
    assert(message_queue_try_send(&queue, &input));
    assert(message_queue_try_receive(&queue, &output) && output == 22u);
    assert(message_queue_try_receive(&queue, &output) && output == 33u);
    assert(message_queue_try_receive(&queue, &output) && output == 44u);
    assert(message_queue_count(&queue) == 0u);
}

static void test_timeout_and_validation(void)
{
    message_queue_t queue;
    uint8_t storage[1];
    uint8_t value = 7u;
    uint8_t output = 0u;

    assert(!message_queue_init(&queue, storage, 0u, 1u));
    assert(!message_queue_init(&queue, storage, 1u, 0u));
    assert(message_queue_init(&queue, storage, 1u, 1u));
    assert(message_queue_receive_timeout(&queue, &output, 10u) ==
           WAIT_TIMEOUT);
    assert(message_queue_send_timeout(&queue, &value, 10u) == WAIT_ACQUIRED);
    assert(message_queue_send_timeout(&queue, &value, 10u) == WAIT_TIMEOUT);
    assert(message_queue_receive_timeout(&queue, &output, 10u) ==
           WAIT_ACQUIRED);
    assert(output == value);
}

int main(void)
{
    test_fifo_and_wraparound();
    test_timeout_and_validation();
    puts("message queue tests passed");
    return 0;
}
