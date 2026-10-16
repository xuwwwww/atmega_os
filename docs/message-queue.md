# Fixed-storage message queues

The kernel message queue moves fixed-size records between native AVR tasks without dynamic allocation. The caller owns the backing array, so its SRAM cost is known at link time.

## Model

A queue contains:

- a caller-provided byte buffer;
- a fixed item size and item count;
- read and write positions;
- a mutex protecting the ring-buffer metadata;
- a counting semaphore for free slots;
- a counting semaphore for available items.

Sending waits for a free slot, locks the queue, copies one record, advances the write position, unlocks, and signals that an item exists. Receiving performs the inverse operation.

## Why storage is supplied by the caller

There is no heap allocator in this project. Static storage avoids fragmentation and makes the final SRAM report meaningful. For example:

```c
static uint8_t storage[4][8];
static message_queue_t queue;

message_queue_init(&queue, storage, 8u, 4u);
```

This queue holds four eight-byte records. Its payload cost is exactly 32 bytes plus the queue control object and synchronization objects.

## Blocking and timeout operations

The API provides blocking, timed, and nonblocking forms. A timeout of zero is suitable for a try operation. Timed waits use wrap-safe millisecond comparisons and return without modifying the queue if the deadline expires.

The current queue API is task context only. It is not safe to call the blocking operations from an interrupt service routine.

## Concurrency rules

- Never access the backing array directly after initialization.
- Do not hold an unrelated mutex while waiting indefinitely on a queue unless the lock order is documented.
- Keep records small on the ATmega328P; copying a large record increases interrupt latency and SRAM pressure.
- Queue storage must remain alive for the entire lifetime of the queue.

## Testing

The host tests cover ring wraparound, full and empty behavior, timeouts, and ordering. Run:

```sh
make test
```

The firmware self-test can additionally exercise queue behavior on real AVR hardware when its build flag is enabled.
