/**
 * @file fault.c
 * @brief TinyOS Exception/Fault Handler — implementation
 *
 * Architecture
 * ------------
 * Each exception vector uses a naked assembly trampoline (FAULT_ENTRY) that:
 *   1. Determines whether the fault frame is on MSP or PSP
 *      (EXC_RETURN bit 2: 0 = MSP, 1 = PSP)
 *   2. Loads the correct stack pointer into R0 (first C argument = frame ptr)
 *   3. Loads the fault type constant into R1 (second C argument)
 *   4. Branches to os_fault_dispatch()
 *
 * os_fault_dispatch() then:
 *   - Reads PC/LR/PSR from the stacked frame
 *   - Reads CFSR/HFSR/MMFAR/BFAR (M3/M4/M7 only; zero on M0/M0+)
 *   - Decodes cause flags from CFSR
 *   - Copies the current task name and tick counter
 *   - Calls the (overridable) os_fault_hook()
 *
 * Cortex-M exception frame (8 words pushed by CPU):
 *   frame[0] = R0
 *   frame[1] = R1
 *   frame[2] = R2
 *   frame[3] = R3
 *   frame[4] = R12
 *   frame[5] = LR  (return address inside faulting task)
 *   frame[6] = PC  (faulting instruction)
 *   frame[7] = xPSR
 */

#include "tinyos/fault.h"
#include "tinyos.h"
#include <string.h>

/*===========================================================================
 * SCB fault-status registers (Cortex-M3/M4/M7 only; not present on M0/M0+)
 *===========================================================================*/

#define REG_CFSR  (*(volatile uint32_t *)0xE000ED28U) /* Configurable Fault Status */
#define REG_HFSR  (*(volatile uint32_t *)0xE000ED2CU) /* HardFault Status          */
#define REG_MMFAR (*(volatile uint32_t *)0xE000ED34U) /* MemManage Fault Address   */
#define REG_BFAR  (*(volatile uint32_t *)0xE000ED38U) /* Bus Fault Address         */

/* CFSR sub-fields */
#define CFSR_MMFSR_MASK  0x000000FFU
#define CFSR_BFSR_MASK   0x0000FF00U
#define CFSR_UFSR_MASK   0xFFFF0000U

/* UFSR bits (bits 16-31 of CFSR) */
#define UFSR_DIVBYZERO  (1U << 25)   /* bit 9 of UFSR = bit 25 of CFSR */
#define UFSR_UNALIGNED  (1U << 24)   /* bit 8 of UFSR = bit 24 of CFSR */
#define UFSR_UNDEFINSTR (1U << 16)   /* bit 0 of UFSR = bit 16 of CFSR */
#define UFSR_INVSTATE   (1U << 17)   /* bit 1 of UFSR = bit 17 of CFSR */

/* MMFSR bits (bits 0-7 of CFSR) */
#define MMFSR_DACCVIOL  (1U <<  1)   /* data access violation           */
#define MMFSR_IACCVIOL  (1U <<  0)   /* instruction access violation    */
#define MMFSR_MMARVALID (1U <<  7)   /* MMFAR holds valid address       */

/* BFSR bits (bits 8-15 of CFSR) */
#define BFSR_PRECISERR  (1U <<  9)   /* precise bus fault, BFAR valid   */
#define BFSR_IMPRECISERR (1U << 10)  /* imprecise bus fault             */
#define BFSR_BFARVALID  (1U << 15)   /* BFAR holds valid address        */

/*===========================================================================
 * Exception frame field indices
 *===========================================================================*/

#define FRAME_R0   0
#define FRAME_R1   1
#define FRAME_R2   2
#define FRAME_R3   3
#define FRAME_R12  4
#define FRAME_LR   5
#define FRAME_PC   6
#define FRAME_XPSR 7

/*===========================================================================
 * State
 *===========================================================================*/

static fault_info_t g_fault;

/*===========================================================================
 * Helper: safe string copy
 *===========================================================================*/

static void fault_strcpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    if (src) {
        while (i < n - 1 && src[i] != '\0') { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

/*===========================================================================
 * C-level dispatcher — called by all four assembly trampolines
 *===========================================================================*/

void os_fault_dispatch(uint32_t *frame, uint32_t type) {
    /* Basic frame fields */
    g_fault.pc  = frame[FRAME_PC];
    g_fault.lr  = frame[FRAME_LR];
    g_fault.psr = frame[FRAME_XPSR];

    /* Fault status registers (M3/M4/M7) — zero on M0/M0+ (reads as 0) */
    g_fault.cfsr = REG_CFSR;
    g_fault.hfsr = REG_HFSR;

    /* Pick up the faulting address when the relevant valid bit is set */
    g_fault.fault_addr = 0;
    if (g_fault.cfsr & MMFSR_MMARVALID)
        g_fault.fault_addr = REG_MMFAR;
    else if (g_fault.cfsr & BFSR_BFARVALID)
        g_fault.fault_addr = REG_BFAR;

    /* Decode cause bitmask from CFSR */
    uint32_t cause = FAULT_CAUSE_NONE;
    if (g_fault.cfsr & UFSR_DIVBYZERO)   cause |= FAULT_CAUSE_DIVZERO;
    if (g_fault.cfsr & UFSR_UNDEFINSTR)  cause |= FAULT_CAUSE_UNDEF_INS;
    if (g_fault.cfsr & UFSR_INVSTATE)    cause |= FAULT_CAUSE_INV_STATE;
    if (g_fault.cfsr & UFSR_UNALIGNED)   cause |= FAULT_CAUSE_UNALIGNED;
    if (g_fault.cfsr & MMFSR_DACCVIOL)   cause |= FAULT_CAUSE_DACC_VIOL;
    if (g_fault.cfsr & MMFSR_IACCVIOL)   cause |= FAULT_CAUSE_IACC_VIOL;
    if (g_fault.cfsr & BFSR_PRECISERR)   cause |= FAULT_CAUSE_BUS_PREC;
    if (g_fault.cfsr & BFSR_IMPRECISERR) cause |= FAULT_CAUSE_BUS_IMPREC;

    g_fault.cause = cause;
    g_fault.type  = (uint8_t)type;
    g_fault.tick  = os_get_tick_count();

    /* Capture the name of the running task */
    const tcb_t *cur = os_task_get_current();
    fault_strcpy(g_fault.task_name,
                 (cur && cur->name[0]) ? cur->name : "?",
                 sizeof(g_fault.task_name));

    /* Mark the record valid last so any concurrent reader sees a complete record */
    g_fault.occurred = true;

    /* Clear the status registers so they don't re-trigger on the next fault */
    REG_CFSR = g_fault.cfsr;   /* write-1-to-clear */
    REG_HFSR = g_fault.hfsr;

    os_fault_hook(&g_fault);
}

/*===========================================================================
 * Default hook — weak so applications can override
 *===========================================================================*/

__attribute__((weak))
void os_fault_hook(const fault_info_t *info) {
    (void)info;
    /* Halt with a debug breakpoint; loop so the PC stays here in the debugger */
    __asm__ volatile("bkpt #2");
    for (;;) {}
}

/*===========================================================================
 * Public API
 *===========================================================================*/

const fault_info_t *os_fault_get_info(void) { return &g_fault; }

void os_fault_clear(void) {
    uint32_t cs = os_enter_critical();
    g_fault.occurred = false;
    os_exit_critical(cs);
}

/*===========================================================================
 * Assembly trampolines
 *
 * Each handler must be naked so we control the stack precisely.
 * The FAULT_ENTRY macro expands to a small Thumb-2 sequence:
 *
 *   tst  lr, #4          ; EXC_RETURN bit 2: 0=MSP, 1=PSP
 *   beq  .use_msp
 *   mrs  r0, psp         ; faulting frame is on PSP (task was using PSP)
 *   b    .call_dispatch
 * .use_msp:
 *   mrs  r0, msp         ; faulting frame is on MSP
 * .call_dispatch:
 *   movs r1, #<type>     ; pass fault type as second argument
 *   b    os_fault_dispatch
 *
 * r0 = pointer to stacked exception frame  (1st C argument)
 * r1 = fault type constant FAULT_*         (2nd C argument)
 *
 * We use "b" (branch, not bl) because os_fault_dispatch() never returns.
 *===========================================================================*/

#define FAULT_ENTRY(type_num)                        \
    __asm__ volatile(                                \
        "tst  lr, #4          \n\t"                  \
        "beq  1f              \n\t"                  \
        "mrs  r0, psp         \n\t"                  \
        "b    2f              \n\t"                  \
        "1:                   \n\t"                  \
        "mrs  r0, msp         \n\t"                  \
        "2:                   \n\t"                  \
        "movs r1, %0          \n\t"                  \
        "b    os_fault_dispatch\n\t"                 \
        :                                            \
        : "i"(type_num)                              \
        :                                            \
    )

__attribute__((naked)) void HardFault_Handler(void)  { FAULT_ENTRY(FAULT_HARD);  }
__attribute__((naked)) void MemManage_Handler(void)  { FAULT_ENTRY(FAULT_MEM);   }
__attribute__((naked)) void BusFault_Handler(void)   { FAULT_ENTRY(FAULT_BUS);   }
__attribute__((naked)) void UsageFault_Handler(void) { FAULT_ENTRY(FAULT_USAGE); }
