# AVR memory and context switching

This chapter explains the native AVR kernel. The RV32 guest interpreter is a second layer and is covered in `os-guide.md`.

## Harvard memory architecture

The ATmega328P has separate program and data address spaces:

- 32 KiB Flash stores AVR instructions and constant data placed in program memory.
- 2 KiB SRAM stores global variables, task stacks, queues, emulator state, uploaded guest images, and the active C call stack.
- 1 KiB EEPROM provides nonvolatile data storage but has limited write endurance.

An AVR instruction cannot be copied into SRAM and executed from there. Native task entry functions must be linked into Flash. The RV32 interpreter is different: guest instruction bytes are data to the AVR, so the interpreter can fetch them from SRAM and emulate them.

## SRAM layout

Conceptually, SRAM contains the following regions:

```text
low address
  .data                 initialized global/static objects
  .bss                  zero-initialized global/static objects
  fixed task stacks     arrays reserved by the application
  queues and VM memory  other static objects
  unused gap
  C/ISR stack           grows downward from RAMEND
high address
```

There is no MMU, virtual memory, or protection boundary. Every native task can address all SRAM. A stack overflow can corrupt another task, a queue, or scheduler state.

## Task control block

Each native task has a task control block, or TCB. The first field is the saved stack pointer because the assembly switch code accesses it directly. The remaining fields describe state such as priority, wake time, stack bounds, and the entry function required for restart.

The stack pointer is not the stack itself. It is a 16-bit address identifying the current top of a task's private stack array.

## Initial stack frame

A task that has never run still needs to look as if it was interrupted earlier. Task creation writes an artificial context frame into the task's stack:

- the return address that begins at the task entry trampoline;
- saved `r0`;
- saved `SREG` with the intended interrupt state;
- saved `r1` through `r31`.

The scheduler loads that saved SP and the assembly restore path pops the frame. Its final `RET` begins the task just as a normal function return would. This is why creating an “empty task” still consumes a complete initial frame.

## Saving a running task

On a cooperative yield, `task_yield()` is called like an ordinary function, so its return address is already on the task stack. The save macro then pushes `r0`, `SREG`, and all 31 remaining registers. It stores the resulting SP in the current TCB.

On preemption, AVR hardware first pushes the interrupted PC. The naked Timer2 ISR constructs the same logical frame. It records an enabled interrupt bit in the saved status because the interrupted task was running with interrupts enabled.

The frame layout, from high to low addresses, is:

```text
return PC, r0, SREG, r1, r2, ... r31
```

AVR `PUSH` decrements SP after storing, so the saved SP points immediately below `r31`.

## The kernel-stack invariant

After a context is saved, scheduler C code must not continue using that task's private stack. Doing so changes the very stack that was just frozen and can overwrite its return address or registers.

The correct switch sequence is:

1. Save the complete task context on its private stack.
2. Save the resulting SP in its TCB.
3. Change SP to the shared kernel stack at `RAMEND` while interrupts are disabled.
4. Run the C scheduler.
5. Load the selected task's saved SP.
6. Restore its registers and status.
7. Finish with `RET` for a cooperative switch or `RETI` for a timer interrupt.

Violating step 3 caused an earlier repeated-boot failure: scheduling C calls corrupted task contexts until a return jumped into invalid code. The dedicated switch to the kernel stack fixed that class of reset.

## Returning, terminating, and restarting

A task entry function is allowed to return. The artificial return path leads to a task-exit trampoline, which marks the task terminated and schedules another task. It must never fall back into arbitrary memory.

Restarting a terminated task rebuilds its initial frame from the stored entry point and stack bounds. Restart discards the old call chain and local variables. It does not recover external resources; a task must not terminate while owning a mutex.

## Sleep and states

`task_sleep(n)` means “do not make this task runnable until at least `n` milliseconds have elapsed.” It does not mean periodic execution. A periodic task implements its own loop and sleeps again after each iteration.

Typical states are ready, running, sleeping, blocked, and terminated. The idle task runs only when no normal task is ready.

## Stack sizing and diagnostics

Stacks are fixed arrays chosen by the application. A watermark pattern is written into unused space at creation; diagnostics count how much remains untouched. This is an estimate of historical maximum use, not a protection mechanism.

Always include room for:

- the initial context frame;
- the deepest function-call chain;
- local variables and compiler spills;
- interrupt entry while the task is running;
- extra margin for unusual error paths.

A reported unused value of zero means the stack is too small or has already overflowed. Bit fields can shrink some TCB metadata, but they cannot compress live register frames, return addresses, or arbitrary stack data safely.

## Native tasks versus RV32 guests

Native tasks execute AVR code directly and have AVR register contexts and private AVR stacks. RV32 guests are byte arrays interpreted by one native supervisor. Their `x0` through `x31`, PC, and guest memory are ordinary AVR data. Saving guest state is therefore a C-structure operation; it does not require replacing the AVR SP for every guest instruction.
