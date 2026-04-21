/**
 * startup.s — Minimal ARM Cortex-M4 startup for QEMU mps2-an385
 *
 * Provides:
 *   - Vector table (placed at 0x00000000 by linker.ld)
 *   - Reset_Handler: copies .data, zeros .bss, calls main
 *   - Default weak handlers for all core exceptions
 */

    .syntax unified
    .thumb

/* ── Symbol declarations ─────────────────────────────────────────────── */
    .extern main
    .extern os_systick_handler   /* from kernel.c */
    .extern SVC_Handler          /* from context_switch.s */
    .extern PendSV_Handler       /* from context_switch.s */

/* ── Linker-script symbols ───────────────────────────────────────────── */
    .extern _estack              /* top of MSP stack  */
    .extern _sidata              /* LMA of .data init values in flash */
    .extern _sdata               /* VMA start of .data in RAM */
    .extern _edata               /* VMA end of .data in RAM */
    .extern _sbss                /* start of .bss in RAM */
    .extern _ebss                /* end of .bss in RAM */

/* ── Vector table ────────────────────────────────────────────────────── */
    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
    .size g_pfnVectors, . - g_pfnVectors

g_pfnVectors:
    .word _estack               /* 0: Initial MSP */
    .word Reset_Handler         /* 1: Reset */
    .word Default_Handler       /* 2: NMI */
    .word HardFault_Handler     /* 3: Hard Fault */
    .word Default_Handler       /* 4: MemManage */
    .word Default_Handler       /* 5: BusFault */
    .word Default_Handler       /* 6: UsageFault */
    .word 0                     /* 7-10: Reserved */
    .word 0
    .word 0
    .word 0
    .word SVC_Handler           /* 11: SVCall */
    .word Default_Handler       /* 12: DebugMon */
    .word 0                     /* 13: Reserved */
    .word PendSV_Handler        /* 14: PendSV */
    .word SysTick_Handler       /* 15: SysTick */

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

/* ── SysTick delegates to kernel ─────────────────────────────────────── */
    .section .text.SysTick_Handler
    .weak SysTick_Handler
    .type SysTick_Handler, %function
SysTick_Handler:
    push {lr}
    bl   os_systick_handler
    pop  {pc}
    .size SysTick_Handler, . - SysTick_Handler

/* ── Default/HardFault handlers ─────────────────────────────────────── */
    .section .text.Default_Handler
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
HardFault_Handler:
    bkpt #0
    b    .
    .size Default_Handler, . - Default_Handler
