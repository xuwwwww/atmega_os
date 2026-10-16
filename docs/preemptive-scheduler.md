# Native preemptive scheduler

The native kernel supports cooperative yields and timer-driven preemption on supported ATmega328P boards. The distributed RV32 job scheduler is a separate layer and should not be confused with native AVR task switching.

## Scheduling model

Each task has a fixed priority and state. The scheduler chooses a ready task at the highest effective priority and uses round-robin selection among peers. Sleeping or blocked tasks are skipped. The idle task is the fallback when nothing else can run.

Two paths enter the scheduler:

- `task_yield()` voluntarily gives up the CPU.
- Timer2 compare interrupt preempts the running task.

Both paths create the same saved register frame, switch to the shared kernel stack, select the next task, load its SP, and restore it. The cooperative path returns with `RET`; the interrupt path returns with `RETI`.

## Tick frequency

`KERNEL_TICK_HZ` controls preemption resolution. A faster tick does not increase CPU speed. It reduces the longest uninterrupted native time slice but increases context-switch overhead. The millisecond API advances only after the configured number of submillisecond ticks.

## Blocking instead of spinning

A task waiting for time or a resource should change state and yield. Busy loops waste the entire processor and prevent low-priority work from progressing.

`task_sleep(delay_ms)` blocks until a deadline. It is a one-shot delay, not a periodic timer. Periodic behavior belongs in a loop or software-timer callback.

## Semaphores

A counting semaphore represents a number of available units. `wait` consumes one unit or blocks; `post` adds a unit and wakes a waiter. Binary use is possible with a maximum count of one, but ownership is not tracked.

Timed and nonblocking waits allow tasks to avoid indefinite blocking. Waiter membership is stored compactly because this kernel has a small compile-time task limit.

Semaphores are appropriate for producer/consumer counts, completion signals, and resource pools. They do not automatically protect a multi-step data structure; combine them with a mutex when exclusive access is required.

## Mutexes and priority inheritance

A mutex has one owner. It is nonrecursive: the owner cannot lock it twice. Unlock validates ownership so one task cannot release another task's lock.

When a high-priority task waits on a mutex owned by a lower-priority task, the owner temporarily inherits the higher priority. This limits priority inversion by helping the owner run and release the resource. Effective priority returns to the appropriate inherited or base value afterward.

Priority inheritance does not prevent deadlock. Two tasks can still acquire locks in opposite order and wait forever. Use a global lock order, short critical sections, or timed acquisition.

## Message queues and event flags

Message queues provide bounded record transfer with caller-owned storage. Event flags let tasks wait for one or more condition bits. Both block through the scheduler rather than polling. See `message-queue.md` for the queue design.

## Interrupt safety

The AVR is 8-bit, so multi-byte scheduler fields are not naturally atomic. Shared state touched by an ISR is read or changed inside short critical sections. A critical section saves `SREG`, executes `CLI`, performs the operation, and restores the original `SREG`; it must not blindly call `SEI` because the caller may already have interrupts disabled.

Keep ISRs short. They should update time, record events, and request scheduling, not print long messages or execute guest workloads.

## Task return and restart

Returning from a task enters the exit trampoline, marks it terminated, and schedules another task. Restart reconstructs the initial context frame and makes the task ready again. Restart does not reset shared peripherals or reclaim locks, so task code must leave resources in a valid state.

## Safe mode

`LGT_SAFE_MODE=1` disables native multi-stack preemption and Timer2 switching. It exists for clone hardware that was unstable with the AVR context-switch implementation. The shell and RV32 supervisor can still run, and RV32 guests can still be time-sliced by the interpreter, but this is not native AVR task preemption.

## Current limitations

- No memory protection or process isolation.
- Fixed task count and fixed stack storage.
- No deadlock detector.
- Priority inheritance is intentionally small and suited to the fixed task table.
- Blocking synchronization APIs are for task context, not arbitrary ISR use.
- A stack overflow can corrupt the whole kernel.

These constraints are deliberate: the goal is to expose operating-system mechanisms on a device with only 2 KiB of SRAM.
