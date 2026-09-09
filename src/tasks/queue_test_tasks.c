#include "queue_test_tasks.h"

#include "../drivers/uart.h"
#include "../kernel/message_queue.h"
#include "../kernel/scheduler.h"

#include <stdint.h>

#define QUEUE_TEST_CAPACITY 3u
#define QUEUE_TEST_BURST_SIZE 8u

static message_queue_t test_queue;
static uint8_t test_storage[QUEUE_TEST_CAPACITY];

void queue_test_init(void)
{
    (void)message_queue_init(&test_queue, test_storage,
                             QUEUE_TEST_CAPACITY, sizeof(uint8_t));
}

void queue_test_producer(void)
{
    uint8_t sequence = 0u;

    for (;;)
    {
        for (uint8_t i = 0u; i < QUEUE_TEST_BURST_SIZE; i++)
        {
            (void)message_queue_send(&test_queue, &sequence);
            sequence++;
        }

        task_sleep(1000u);
    }
}

void queue_test_consumer(void)
{
    uint8_t expected = 0u;
    uint8_t received_count = 0u;

    for (;;)
    {
        uint8_t value;
        wait_result_t result =
            message_queue_receive_timeout(&test_queue, &value, 200u);

        if (result == WAIT_TIMEOUT)
        {
            uart_write_char('T');
            continue;
        }

        if (result != WAIT_ACQUIRED || value != expected)
        {
            uart_write_char('E');
            continue;
        }

        expected++;
        received_count++;

        if (received_count >= QUEUE_TEST_BURST_SIZE)
        {
            received_count = 0u;
            uart_write_char('Q');
        }
    }
}
