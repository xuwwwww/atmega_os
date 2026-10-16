# Timer ownership and kernel time

The ATmega328P has three hardware timers. This project assigns them differently by firmware profile; they are not interchangeable global clocks.

## Native-kernel profile

Timer2 drives the native preemptive scheduler when `LGT_SAFE_MODE=0`. It runs in CTC mode with a `/64` prescaler. `OCR2A` is calculated from `F_CPU` and `KERNEL_TICK_HZ`.

At 16 MHz and the default 1 kHz tick:

```text
16,000,000 / 64 / 1,000 = 250 counts
OCR2A = 250 - 1 = 249
```

The compare interrupt saves the interrupted task, switches to the kernel stack, updates time and scheduling state, selects a task, restores its context, and returns with `RETI`.

The code can use a faster tick only when the requested frequency divides the timer clock exactly and still fits in an 8-bit compare register. A faster tick improves scheduling resolution but spends more CPU time saving and restoring contexts. It does not make the processor execute guest instructions faster.

## Distributed-node profile

The coordinator and worker firmware use Timer0 as a 1 ms wall clock. Timer1 is used as a polled time-slice source for RV32 guest execution. Polling Timer1 avoids a large AVR interrupt frame while the interpreter is active.

The Arduino core is not linked, so its usual `millis()` and PWM ownership rules do not apply. This firmware configures the peripherals directly.

## LGT safe mode

Some clone boards were unstable during AVR context switching and timer interrupts. `LGT_SAFE_MODE=1` stops Timer2 and uses a single native stack with calibrated delay-based time advancement. This mode sacrifices native preemption to provide a stable supervisor for the shell and RV32 interpreter.

## Atomic access

The millisecond counter is 32 bits, while the AVR is 8 bits. Reading it requires a short critical section: save `SREG`, disable interrupts, copy the value, then restore `SREG`. Incrementing it inside the timer ISR is already atomic with respect to other interrupts because the AVR enters the ISR with interrupts disabled.

Elapsed-time checks use unsigned subtraction:

```c
if ((uint32_t)(now - started_at) >= delay_ms) {
    /* deadline reached */
}
```

This remains correct when the counter wraps, provided the requested interval is less than half of the 32-bit range.
