/**
 * startup.s — Minimal ARM Cortex-M4 startup for QEMU mps2-an385
 *
 * Provides:
 *   - Vector table (placed at 0x00000000 by linker.ld)
 *   - Reset_Handler: copies .data, zeros .bss, calls main
 *   - Default weak handler for truly unhandled exceptions (NMI, DebugMon)
 *
 * Fault handlers (HardFault, MemManage, BusFault, UsageFault) are strong
 * symbols defined in fault.c via naked assembly trampolines that capture
 * the exception frame and call os_fault_dispatch().  The vector table
 * entries here reference those strong symbols directly so the diagnostic
 * handler is always invoked — the old Default_Handler stub was a silent
 * dead-end that discarded all fault information.
 */

    .syntax unified
    .thumb

/* ── External symbols ────────────────────────────────────────────────── */
    .extern main
    .extern SVC_Handler           /* context_switch.s */
    .extern PendSV_Handler        /* context_switch.s */
    .extern SysTick_Handler       /* kernel.c         */
    .extern HardFault_Handler     /* fault.c          */
    .extern MemManage_Handler     /* fault.c          */
    .extern BusFault_Handler      /* fault.c          */
    .extern UsageFault_Handler    /* fault.c          */

/* ── Linker-script symbols ───────────────────────────────────────────── */
    .extern _estack
    .extern _sidata
    .extern _sdata
    .extern _edata
    .extern _sbss
    .extern _ebss

/* ── Vector table ────────────────────────────────────────────────────── */
    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
    .size g_pfnVectors, . - g_pfnVectors

g_pfnVectors:
    .word _estack               /* 0:  Initial MSP                           */
    .word Reset_Handler         /* 1:  Reset                                 */
    .word Default_Handler       /* 2:  NMI                                   */
    .word HardFault_Handler     /* 3:  HardFault  — fault.c                  */
    .word MemManage_Handler     /* 4:  MemManage  — fault.c (was Default!)   */
    .word BusFault_Handler      /* 5:  BusFault   — fault.c (was Default!)   */
    .word UsageFault_Handler    /* 6:  UsageFault — fault.c (was Default!)   */
    .word 0                     /* 7-10: Reserved                            */
    .word 0
    .word 0
    .word 0
    .word SVC_Handler           /* 11: SVCall  — context_switch.s            */
    .word Default_Handler       /* 12: DebugMon                              */
    .word 0                     /* 13: Reserved                              */
    .word PendSV_Handler        /* 14: PendSV  — context_switch.s            */
    .word SysTick_Handler       /* 15: SysTick — kernel.c                    */

/* ── Reset handler ───────────────────────────────────────────────────── */
    .section .text.Reset_Handler
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* Copy .data from flash to RAM */
    ldr  r0, =_sdata
    ldr  r1, =_edata
    ldr  r2, =_sidata
    b    .Lcopy_data_check
.Lcopy_data_loop:
    ldr  r3, [r2], #4
    str  r3, [r0], #4
.Lcopy_data_check:
    cmp  r0, r1
    bcc  .Lcopy_data_loop

    /* Zero .bss */
    ldr  r0, =_sbss
    ldr  r1, =_ebss
    movs r2, #0
    b    .Lzero_bss_check
.Lzero_bss_loop:
    str  r2, [r0], #4
.Lzero_bss_check:
    cmp  r0, r1
    bcc  .Lzero_bss_loop

    /* Call main */
    bl   main

    /* Should never reach here */
    b    .
    .size Reset_Handler, . - Reset_Handler

/* ── Default handler (NMI, DebugMon, and any unregistered exception) ─── */
    .section .text.Default_Handler
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    bkpt #0
    b    .
    .size Default_Handler, . - Default_Handler
