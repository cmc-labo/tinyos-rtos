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
 *        │  R10     │  │  saved by PendSV_Handler (manually)
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
 *
 * Preemptive scheduling overview
 * --------------------------------
 * SysTick  : manages tick counter and time-slice countdown.
 *            When a switch is needed it pends PendSV via os_pend_sv().
 * PendSV   : lowest-priority exception — fires AFTER all other ISRs.
 *            Performs the actual PSP swap (save R4-R11, call C helper,
 *            restore R4-R11). This cleanly decouples scheduling policy
 *            (kernel.c) from the hardware-dependent register save/restore.
 * os_pend_sv(): sets ICSR.PENDSVSET from any context (Thread or Handler).
 */

    .syntax unified
    .thumb

/* -----------------------------------------------------------------------
 * os_pend_sv()
 *
 * Requests a PendSV exception by setting bit 28 (PENDSVSET) in the
 * Interrupt Control and State Register (ICSR, 0xE000ED04).
 *
 * Safe to call from both Thread mode and Handler mode (ISR).
 * PendSV will fire AFTER all currently-active exceptions complete,
 * guaranteeing the context switch happens at the lowest-priority moment.
 * ----------------------------------------------------------------------- */

    .global os_pend_sv
    .type   os_pend_sv, %function
os_pend_sv:
    LDR     R0, =0xE000ED04         /* ICSR address                          */
    LDR     R1, =0x10000000         /* bit 28 = PENDSVSET                    */
    STR     R1, [R0]                /* write → PendSV becomes pending        */
    DSB                             /* ensure write completes before return  */
    ISB                             /* flush pipeline                        */
    BX      LR
    .size os_pend_sv, . - os_pend_sv


/* -----------------------------------------------------------------------
 * PendSV_Handler  — the actual context switch
 *
 * Runs at the lowest interrupt priority (configured in os_start()).
 * On entry (Handler mode, MSP; CPU has auto-saved old task's
 * {R0-R3, R12, LR, PC, xPSR} to its PSP):
 *
 *   1. Save R4-R11 below the auto-saved frame on old task's PSP.
 *   2. Call os_pendsv_switch(old_psp) → C decides old/new task,
 *      returns new task's stack_ptr (pointing at its saved R4-R11).
 *   3. Restore R4-R11 from new task's PSP.
 *   4. Update PSP to point at new task's auto-save frame.
 *   5. EXC_RETURN → CPU pops {R0-R3, R12, LR, PC, xPSR} from new PSP.
 *
 * C prototype (kernel.c):
 *   uint32_t *os_pendsv_switch(uint32_t *old_psp);
 * ----------------------------------------------------------------------- */

    .global PendSV_Handler
    .type   PendSV_Handler, %function
PendSV_Handler:
    /* Disable interrupts during the register manipulation to prevent
     * a higher-priority ISR from corrupting our PSP work.           */
    CPSID   I

    /* Save callee-saved registers of the outgoing task.
     * R0 ← PSP of old task (after CPU's auto-save frame).          */
    MRS     R0, PSP
    STMDB   R0!, {R4-R11}           /* push R4-R11 below auto-frame  */

    /* Call C scheduler to commit old PSP and obtain new PSP.
     * R0 (first argument) = updated PSP after saving R4-R11.
     * Return value (R0)   = new task's stack_ptr (at its R4-R11).  */
    PUSH    {LR}                    /* preserve EXC_RETURN value      */
    BL      os_pendsv_switch        /* R0 = os_pendsv_switch(old_psp) */
    POP     {LR}                    /* restore EXC_RETURN             */

    /* Restore callee-saved registers of the incoming task.          */
    LDMIA   R0!, {R4-R11}           /* pop R4-R11; R0 advances past them */

    /* Point PSP at the new task's auto-save frame (R0-R3/R12/LR/PC/xPSR).
     * EXC_RETURN will pop these automatically.                      */
    MSR     PSP, R0

    CPSIE   I                       /* re-enable interrupts           */
    BX      LR                      /* EXC_RETURN → new task resumes  */
    .size PendSV_Handler, . - PendSV_Handler


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
    SVC     #0                      /* → SVC_Handler; R0 preserved in frame  */
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
    LDR     R0, [R1, #0]            /* frame[0] = original R0 = stack_ptr    */

    /* Restore R4-R11 and advance R0 past them to the exception frame.
     * This mirrors the PendSV tail so both paths leave PSP pointing
     * at the CPU auto-save frame {R0-R3, R12, LR, PC, xPSR}.          */
    LDMIA   R0!, {R4-R11}

    MSR     PSP, R0                 /* PSP now at the exception frame         */

    MOV     R0, #2
    MSR     CONTROL, R0             /* CONTROL.SPSEL = 1 → Thread uses PSP   */
    ISB                             /* flush pipeline after CONTROL write     */

    /* EXC_RETURN = 0xFFFFFFFD → Thread/PSP.
     * CPU pops {R0-R3, R12, LR, PC, xPSR} from PSP → first task runs.  */
    LDR     LR, =0xFFFFFFFD
    BX      LR
    .size SVC_Handler, . - SVC_Handler
