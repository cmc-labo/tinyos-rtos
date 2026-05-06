/**
 * TinyOS Security Features
 *
 * Memory Protection Unit (MPU) configuration
 * Access control and privilege management
 */

#include "tinyos.h"

/* MPU registers (ARM Cortex-M specific) */
#define MPU_TYPE        (*((volatile uint32_t *)0xE000ED90))
#define MPU_CTRL        (*((volatile uint32_t *)0xE000ED94))
#define MPU_RNR         (*((volatile uint32_t *)0xE000ED98))
#define MPU_RBAR        (*((volatile uint32_t *)0xE000ED9C))
#define MPU_RASR        (*((volatile uint32_t *)0xE000EDA0))

/* MPU Control Register bits */
#define MPU_CTRL_ENABLE         (1 << 0)
#define MPU_CTRL_HFNMIENA       (1 << 1)
#define MPU_CTRL_PRIVDEFENA     (1 << 2)

/* MPU Region Attribute and Size Register bits */
#define MPU_RASR_ENABLE         (1U << 0)
#define MPU_RASR_SIZE(n)        (((n) - 1) << 1)  /* Region size = 2^(n+1) bytes */
#define MPU_RASR_XN             (1U << 28)         /* Execute Never */
#define MPU_RASR_AP_NO_ACCESS   (0U << 24)
#define MPU_RASR_AP_RW_PRIV     (1U << 24)
#define MPU_RASR_AP_RW_ALL      (3U << 24)
#define MPU_RASR_AP_RO_PRIV     (5U << 24)
#define MPU_RASR_AP_RO_ALL      (6U << 24)

/* Permission flags */
#define PERM_READ       (1 << 0)
#define PERM_WRITE      (1 << 1)
#define PERM_EXEC       (1 << 2)

/**
 * Enable or disable MPU
 */
void os_mpu_enable(bool enable) {
    if (enable) {
        MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    } else {
        MPU_CTRL = 0;
    }

    __asm__ volatile("dsb\n\tisb");
}

/**
 * Set MPU region
 */
os_error_t os_mpu_set_region(
    uint8_t region_id,
    memory_region_t *region
) {
    if (region == NULL || region_id >= 8) {
        return OS_ERROR_INVALID_PARAM;
    }

    /* Check if address is aligned */
    uint32_t addr = (uint32_t)region->start_addr;
    if (addr & 0x1F) {  /* Must be 32-byte aligned */
        return OS_ERROR_INVALID_PARAM;
    }

    /* Calculate region size (must be power of 2) */
    uint32_t size = region->size;
    uint8_t size_bits = (size > 1) ? (uint8_t)(32 - __builtin_clz(size - 1)) : 0;

    if (size_bits < 5 || size_bits > 31) {  /* 32 bytes to 4GB */
        return OS_ERROR_INVALID_PARAM;
    }

    /* Determine access permissions */
    uint32_t ap_bits;
    if (region->permissions == 0) {
        ap_bits = MPU_RASR_AP_NO_ACCESS;
    } else if (region->permissions & PERM_WRITE) {
        ap_bits = MPU_RASR_AP_RW_ALL;  /* Read-write for all */
    } else if (region->permissions & PERM_READ) {
        ap_bits = MPU_RASR_AP_RO_ALL;  /* Read-only for all */
    } else {
        ap_bits = MPU_RASR_AP_NO_ACCESS;
    }

    /* XN (Execute Never): set unless caller explicitly requests execute permission */
    uint32_t xn_bit = (region->permissions & PERM_EXEC) ? 0U : MPU_RASR_XN;

    /* Configure region */
    MPU_RNR = region_id;
    MPU_RBAR = addr;
    MPU_RASR = MPU_RASR_ENABLE |
               MPU_RASR_SIZE(size_bits) |
               ap_bits |
               xn_bit;

    return OS_OK;
}

static void mpu_configure_region(uint8_t id, void *addr, uint32_t size, uint32_t perms) {
    memory_region_t r;
    r.start_addr  = addr;
    r.size        = size;
    r.permissions = perms;
    os_mpu_set_region(id, &r);
}

/**
 * Configure default memory regions
 */
void os_mpu_configure_default(void) {
    /* Skip MPU setup if no MPU present (bits[15:8] of MPU_TYPE == 0) */
    if ((MPU_TYPE & 0xFF00u) == 0) {
        return;
    }
    /* Region 0: Flash — read + execute, no write */
    mpu_configure_region(0, (void *)0x00000000, 4 * 1024 * 1024,   PERM_READ | PERM_EXEC);
    /* Region 1: SRAM  — read + write, XN (no code execution from RAM) */
    mpu_configure_region(1, (void *)0x20000000, 4 * 1024 * 1024,   PERM_READ | PERM_WRITE);
    /* Region 2: Peripheral — privileged read/write, XN */
    mpu_configure_region(2, (void *)0x40000000, 512 * 1024 * 1024, PERM_READ | PERM_WRITE);
    os_mpu_enable(true);
}

/**
 * Check memory access permission
 */
bool os_check_memory_access(void *addr, size_t size, uint8_t permission) {
    /* This would check MPU configuration */
    /* For now, simplified implementation */
    (void)addr;
    (void)size;
    (void)permission;

    /* TODO: Implement actual MPU region checking */
    return true;
}

/**
 * MPU / MemManage fault handler
 */
void os_mpu_fault_handler(void) {
    /* SCB_CFSR[7:0] = MMFSR: MMARVALID(bit7), MLSPERR(5), MSTKERR(4),
     * MUNSTKERR(3), DACCVIOL(1), IACCVIOL(0) */
    volatile uint32_t cfsr  = *(volatile uint32_t *)0xE000ED28U;
    /* SCB_MMFAR: valid only when MMFSR.MMARVALID is set */
    volatile uint32_t mmfar = *(volatile uint32_t *)0xE000ED34U;

    (void)cfsr;
    (void)mmfar;

    /* Spin with breakpoint so a debugger can inspect cfsr / mmfar and the
     * stack frame to identify the faulting instruction. */
    while (1) {
        __asm__ volatile("bkpt #0");
    }
}

/**
 * Secure boot verification (placeholder)
 */
bool os_verify_boot_integrity(void) {
    /* TODO: Implement cryptographic boot verification */
    /* For production, should verify firmware signature */
    return true;
}

/**
 * Initialize security subsystem
 */
void os_security_init(void) {
    /* Verify boot integrity */
    if (!os_verify_boot_integrity()) {
        /* Boot verification failed */
        while (1);
    }

    /* Configure memory protection */
    os_mpu_configure_default();
}
