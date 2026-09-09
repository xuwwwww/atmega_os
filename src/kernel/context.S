#include <avr/io.h>

.global task_yield
.global scheduler_start
.global TIMER2_COMPA_vect
.extern g_current_task
.extern scheduler_select_next
.extern scheduler_tick

/*
 * Stack after CONTEXT_SAVE, from high address to low address:
 * return PC, r0, SREG, r1, r2, ... r31.
 * SP points one byte below r31 because AVR PUSH is post-decrement.
 */
.macro CONTEXT_SAVE
    push r0
    in   r0, _SFR_IO_ADDR(SREG)
    cli
    push r0
    CONTEXT_SAVE_REGISTERS
.endm

/* Interrupt entry clears SREG.I in hardware.  The interrupted task was
 * necessarily running with I set, so put that logical value in its frame. */
.macro CONTEXT_SAVE_ISR
    push r0
    in   r0, _SFR_IO_ADDR(SREG)
    set
    bld  r0, SREG_I
    cli
    push r0
    CONTEXT_SAVE_REGISTERS
.endm

.macro CONTEXT_SAVE_REGISTERS
    push r1
    clr  r1
    push r2
    push r3
    push r4
    push r5
    push r6
    push r7
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push r16
    push r17
    push r18
    push r19
    push r20
    push r21
    push r22
    push r23
    push r24
    push r25
    push r26
    push r27
    push r28
    push r29
    push r30
    push r31

    lds  r26, g_current_task
    lds  r27, g_current_task + 1
    in   r0, _SFR_IO_ADDR(SPL)
    st   X+, r0
    in   r0, _SFR_IO_ADDR(SPH)
    st   X+, r0
.endm

.macro CONTEXT_LOAD_SP
    lds  r26, g_current_task
    lds  r27, g_current_task + 1
    ld   r28, X+
    ld   r29, X+
    out  _SFR_IO_ADDR(SPH), r29
    out  _SFR_IO_ADDR(SPL), r28
.endm

.macro CONTEXT_RESTORE_REGISTERS
    pop  r31
    pop  r30
    pop  r29
    pop  r28
    pop  r27
    pop  r26
    pop  r25
    pop  r24
    pop  r23
    pop  r22
    pop  r21
    pop  r20
    pop  r19
    pop  r18
    pop  r17
    pop  r16
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  r7
    pop  r6
    pop  r5
    pop  r4
    pop  r3
    pop  r2
    pop  r1
    pop  r0
    out  _SFR_IO_ADDR(SREG), r0
    pop  r0
.endm

.macro CONTEXT_RESTORE
    CONTEXT_LOAD_SP
    CONTEXT_RESTORE_REGISTERS
.endm

.macro CONTEXT_LOAD_KERNEL_STACK
    /* main() never resumes after scheduler_start(). Reuse its former stack
     * as the non-reentrant kernel stack while interrupts are disabled. */
    ldi  r26, lo8(RAMEND)
    ldi  r27, hi8(RAMEND)
    out  _SFR_IO_ADDR(SPH), r27
    out  _SFR_IO_ADDR(SPL), r26
.endm

.type task_yield, @function
task_yield:
    CONTEXT_SAVE
    CONTEXT_LOAD_KERNEL_STACK
    call scheduler_select_next
    CONTEXT_RESTORE
    ret
.size task_yield, .-task_yield

.type scheduler_start, @function
scheduler_start:
    cli
    CONTEXT_RESTORE
    ret
.size scheduler_start, .-scheduler_start

/* Naked Timer2 ISR: the hardware-pushed PC is part of the task context, so a
 * single RETI can resume either an interrupted or a never-run task. */
.type TIMER2_COMPA_vect, @function
TIMER2_COMPA_vect:
    CONTEXT_SAVE_ISR
    CONTEXT_LOAD_KERNEL_STACK
    call scheduler_tick
    CONTEXT_RESTORE
    reti
.size TIMER2_COMPA_vect, .-TIMER2_COMPA_vect
