/**
 * @file context_switch.s
 * @brief ARM Cortex-M Context Switch — Thumb-2 Assembly
 *
 * Thread model
 * ------------
 *   Thread mode  (tasks)    : uses PSP  (CONTROL.SPSEL = 1)
 *   Handler mode (ISR/RTOS) : uses MSP
 *
 * Stack layout for a suspended task (addresses increase upward):
 *
 *   HIGH ┌──────────┐
 *        │  xPSR    │ ─┐
 *        │  PC      │  │  auto-saved by CPU on exception entry
 *        │  LR      │  │  (EXC_RETURN restores these)
 *        │  R12     │  │
 *        │  R3      │  │
 *        │  R2      │  │
 *        │  R1      │  │
 *        │  R0      │ ─┘
 *        ├──────────┤
 *        │  R11     │ ─┐
 *        │  R10     │  │  saved by context_switch() (manually)
 *        │  R9      │  │
 *        │  R8      │  │
 *        │  R7      │  │
 *        │  R6      │  │
 *        │  R5      │  │
 *        │  R4      │ ─┘
 *   LOW  └──────────┘ ← task->stack_ptr (saved/restored here)
 *
 * Exception frame (8 words) is placed by the CPU *above* R4-R11.
 * On the very first run the initial fake frame occupies the top
 * 8 words and there is no R4-R11 block; os_start_first_task()
 * handles that case separately.
 */

    .syntax unified
    .thumb

/* -----------------------------------------------------------------------
 * context_switch(uint32_t **old_sp, uint32_t **new_sp)
 *
 *   R0 = &old_task->stack_ptr
 *   R1 = &new_task->stack_ptr
 *
 * Called as a normal C function from inside SysTick_Handler
 * (Handler mode; MSP is in use).  At this point the CPU has already
 * auto-saved {R0-R3, R12, LR, PC, xPSR} of the OLD task onto its PSP.
 *
 * This routine:
 *   1. Manually saves R4-R11 below the auto-saved frame on OLD PSP.
 *   2. Stores the updated PSP into *old_sp.
 *   3. Loads the NEW PSP from *new_sp.
 *   4. Manually restores R4-R11 from NEW PSP.
 *   5. Updates PSP so the auto-save frame is next.
 *   6. Returns to SysTick_Handler (BX LR → MSP/Handler context).
 *      SysTick then does its own EXC_RETURN which pops the NEW task's
 *      auto-save frame → NEW task resumes.
 * ----------------------------------------------------------------------- */

    .global context_switch
    .type   context_switch, %function
context_switch:
    /* --- save old task ------------------------------------------------- */
    MRS     R2, PSP                 /* R2 = old task's current PSP          */
    STMDB   R2!, {R4-R11}          /* push R4-R11 downward; R2 updated      */
    STR     R2, [R0]               /* *old_sp = updated PSP                 */

    /* --- restore new task ----------------------------------------------- */
    LDR     R2, [R1]               /* R2 = new task's saved stack_ptr       */
    LDMIA   R2!, {R4-R11}          /* pop R4-R11 upward; R2 updated         */
    MSR     PSP, R2                /* PSP now points at new task's auto-frame*/

    /* --- return to ISR -------------------------------------------------- */
    BX      LR                     /* back to SysTick; EXC_RETURN follows   */
    .size context_switch, . - context_switch


/* -----------------------------------------------------------------------
 * os_start_first_task(uint32_t *sp)
 *
 *   R0 = first task's stack_ptr  (points at fake 8-word exception frame)
 *
 * Called ONCE from os_start() in Thread mode.  Uses SVC to enter
 * Handler mode so that EXC_RETURN can be used to jump to the first task
 * in Thread/PSP mode — the canonical ARM Cortex-M RTOS launch pattern.
 * ----------------------------------------------------------------------- */

    .global os_start_first_task
    .type   os_start_first_task, %function
os_start_first_task:
    SVC     #0                     /* → SVC_Handler; R0 preserved in frame  */
    /* unreachable */
    B       .
    .size os_start_first_task, . - os_start_first_task


/* -----------------------------------------------------------------------
 * SVC_Handler  (handles SVC #0 from os_start_first_task)
 *
 * On entry (Handler mode, MSP):
 *   MSP  → exception frame [ R0, R1, R2, R3, R12, LR, PC, xPSR ]
 *   frame[0] == original R0 == first task's stack_ptr
 *
 * Steps:
 *   1. Read the first task's stack_ptr from the MSP frame.
 *   2. Load PSP with it.
 *   3. Set CONTROL.SPSEL = 1  (Thread mode uses PSP from now on).
 *   4. Zero callee-saved registers for a clean initial state.
 *   5. EXC_RETURN 0xFFFFFFFD → Thread/PSP, CPU pops fake frame → task runs.
 * ----------------------------------------------------------------------- */

    .global SVC_Handler
    .type   SVC_Handler, %function
SVC_Handler:
    MRS     R1, MSP
    LDR     R0, [R1, #0]           /* frame[0] = original R0 = first SP     */

    MSR     PSP, R0                /* set first task's PSP                  */

    MOV     R0, #2
    MSR     CONTROL, R0            /* CONTROL.SPSEL = 1 → Thread uses PSP   */
    ISB                            /* flush pipeline after CONTROL write     */

    /* Zero callee-saved registers so the first task starts cleanly */
    MOV     R4,  #0
    MOV     R5,  #0
    MOV     R6,  #0
    MOV     R7,  #0
    MOV     R8,  #0
    MOV     R9,  #0
    MOV     R10, #0
    MOV     R11, #0

    /* EXC_RETURN = 0xFFFFFFFD
     *   bits[31:4] = 0xFFFFFFF  (EXC_RETURN magic)
     *   bit  3     = 1          return to Thread mode
     *   bit  2     = 1          return using PSP
     *   bit  1     = 0          (reserved)
     *   bit  0     = 1          (EXC_RETURN marker, must be 1)
     *
     * CPU pops {R0-R3, R12, LR, PC, xPSR} from PSP → first task runs.
     */
    LDR     LR, =0xFFFFFFFD
    BX      LR
    .size SVC_Handler, . - SVC_Handler
