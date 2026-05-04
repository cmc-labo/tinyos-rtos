/*
 * linker.ld — ARM Cortex-M4 linker script for QEMU mps2-an385
 *
 * Memory map (MPS2 AN385 / AN386):
 *   Flash : 0x00000000, 4 MB  (ZBT SSRAM1 — execute in place)
 *   RAM   : 0x20000000, 4 MB  (ZBT SSRAM2/3)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

/* Top of stack = end of RAM */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* Vector table must be at 0x00000000 */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* Initialised data: stored in FLASH, copied to RAM at startup */
    _sidata = LOADADDR(.data);
    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* Zero-initialised data */
    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Non-initialized data — NOT zeroed by Reset_Handler.
     * Content survives a soft reset, making it suitable for post-mortem
     * crash records (e.g. os_overflow_record_t). */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit*)
        . = ALIGN(4);
    } > RAM

    /* Stack (grows down from _estack) */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _sstack = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.note*)
        *(.comment*)
    }
}
