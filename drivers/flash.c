/**
 * @file flash.c
 * @brief Flash Memory Driver Implementation
 *
 * RAM-based simulation for testing (can be overridden with platform-specific implementations)
 */

#include "flash.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * RAM-based Flash Simulation
 * ============================================================================ */

static uint8_t simulated_flash[FLASH_TOTAL_SIZE];
static bool flash_initialized = false;
static bool write_protection_enabled = false;
static uint32_t protected_start = 0;
static uint32_t protected_size = 0;

/* ============================================================================
 * Error Strings
 * ============================================================================ */

static const char *error_strings[] = {
    "OK",
    "Invalid parameter",
    "Not aligned",
    "Out of range",
    "Write protected",
    "Erase failed",
    "Write failed",
    "Verify failed",
    "Busy",
    "Timeout"
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static bool is_address_valid(uint32_t address, size_t size) {
    return (address + size) <= FLASH_TOTAL_SIZE;
}

static bool is_sector_aligned(uint32_t address) {
    return (address % FLASH_SECTOR_SIZE) == 0;
}

static bool is_write_protected_range(uint32_t address, size_t size) {
    if (!write_protection_enabled) {
        return false;
    }

    uint32_t end = address + size;
    uint32_t protected_end = protected_start + protected_size;

    return !((end <= protected_start) || (address >= protected_end));
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

flash_error_t flash_init(void) {
    if (flash_initialized) {
        return FLASH_OK;
    }

    /* Initialize simulated flash to 0xFF (erased state) */
    memset(simulated_flash, 0xFF, FLASH_TOTAL_SIZE);

    /* Call platform-specific initialization if available */
    flash_error_t err = platform_flash_init();
    if (err != FLASH_OK) {
        return err;
    }

    flash_initialized = true;
    return FLASH_OK;
}

flash_error_t flash_get_info(flash_info_t *info) {
    if (info == NULL) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    info->total_size = FLASH_TOTAL_SIZE;
    info->page_size = FLASH_PAGE_SIZE;
    info->sector_size = FLASH_SECTOR_SIZE;
    info->write_alignment = 4;  /* 4-byte alignment */
    info->write_protection_enabled = write_protection_enabled;

    return FLASH_OK;
}

/* ============================================================================
 * Read Operations
 * ============================================================================ */

flash_error_t flash_read(uint32_t address, void *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    if (!flash_initialized) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    if (!is_address_valid(address, size)) {
        return FLASH_ERROR_OUT_OF_RANGE;
    }

    /* Call platform-specific read if available */
    flash_error_t err = platform_flash_read(address, buffer, size);
    if (err == FLASH_OK) {
        return FLASH_OK;
    }

    /* Use simulated flash */
    memcpy(buffer, &simulated_flash[address], size);
    return FLASH_OK;
}

flash_error_t flash_read_byte(uint32_t address, uint8_t *value) {
    return flash_read(address, value, 1);
}

flash_error_t flash_read_word(uint32_t address, uint32_t *value) {
    if ((address % 4) != 0) {
        return FLASH_ERROR_NOT_ALIGNED;
    }
    return flash_read(address, value, 4);
}

/* ============================================================================
 * Write Operations
 * ============================================================================ */

flash_error_t flash_write(uint32_t address, const void *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    if (!flash_initialized) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    if (!is_address_valid(address, size)) {
        return FLASH_ERROR_OUT_OF_RANGE;
    }

    if (is_write_protected_range(address, size)) {
        return FLASH_ERROR_WRITE_PROTECTED;
    }

    /* Call platform-specific write if available */
    flash_error_t err = platform_flash_write(address, buffer, size);
    if (err == FLASH_OK) {
        return FLASH_OK;
    }

    /* Use simulated flash */
    memcpy(&simulated_flash[address], buffer, size);
    return FLASH_OK;
}

flash_error_t flash_write_byte(uint32_t address, uint8_t value) {
    return flash_write(address, &value, 1);
}

flash_error_t flash_write_word(uint32_t address, uint32_t value) {
    if ((address % 4) != 0) {
        return FLASH_ERROR_NOT_ALIGNED;
    }
    return flash_write(address, &value, 4);
}

flash_error_t flash_write_verify(uint32_t address, const void *buffer, size_t size) {
    flash_error_t err;
    uint8_t *verify_buffer;

    /* Write data */
    err = flash_write(address, buffer, size);
    if (err != FLASH_OK) {
        return err;
    }

    /* Allocate verify buffer */
    verify_buffer = (uint8_t *)os_malloc(size);
    if (verify_buffer == NULL) {
        return FLASH_ERROR_VERIFY_FAILED;
    }

    /* Read back and verify */
    err = flash_read(address, verify_buffer, size);
    if (err != FLASH_OK) {
        os_free(verify_buffer);
        return err;
    }

    if (memcmp(buffer, verify_buffer, size) != 0) {
        os_free(verify_buffer);
        return FLASH_ERROR_VERIFY_FAILED;
    }

    os_free(verify_buffer);
    return FLASH_OK;
}

/* ============================================================================
 * Erase Operations
 * ============================================================================ */

flash_error_t flash_erase_sector(uint32_t address) {
    if (!flash_initialized) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    /* Align to sector boundary */
    uint32_t sector_address = (address / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    if (!is_address_valid(sector_address, FLASH_SECTOR_SIZE)) {
        return FLASH_ERROR_OUT_OF_RANGE;
    }

    if (is_write_protected_range(sector_address, FLASH_SECTOR_SIZE)) {
        return FLASH_ERROR_WRITE_PROTECTED;
    }

    /* Call platform-specific erase if available */
    flash_error_t err = platform_flash_erase_sector(sector_address);
    if (err == FLASH_OK) {
        return FLASH_OK;
    }

    /* Erase sector (set to 0xFF) */
    memset(&simulated_flash[sector_address], 0xFF, FLASH_SECTOR_SIZE);
    return FLASH_OK;
}

flash_error_t flash_erase_range(uint32_t start_address, size_t size) {
    if (!flash_initialized) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    /* Calculate number of sectors to erase */
    uint32_t start_sector = (start_address / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    uint32_t end_address = start_address + size;
    uint32_t end_sector = ((end_address + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    /* Erase all sectors in range */
    for (uint32_t addr = start_sector; addr < end_sector; addr += FLASH_SECTOR_SIZE) {
        flash_error_t err = flash_erase_sector(addr);
        if (err != FLASH_OK) {
            return err;
        }
    }

    return FLASH_OK;
}

flash_error_t flash_erase_chip(void) {
    if (!flash_initialized) {
        return FLASH_ERROR_INVALID_PARAM;
    }

    if (write_protection_enabled) {
        return FLASH_ERROR_WRITE_PROTECTED;
    }

    /* Erase entire flash */
    memset(simulated_flash, 0xFF, FLASH_TOTAL_SIZE);
    return FLASH_OK;
}

/* ============================================================================
 * Protection
 * ============================================================================ */

flash_error_t flash_enable_write_protection(uint32_t start_address, size_t size) {
    if (!is_address_valid(start_address, size)) {
        return FLASH_ERROR_OUT_OF_RANGE;
    }

    protected_start = start_address;
    protected_size = size;
    write_protection_enabled = true;

    return FLASH_OK;
}

flash_error_t flash_disable_write_protection(void) {
    write_protection_enabled = false;
    protected_start = 0;
    protected_size = 0;

    return FLASH_OK;
}

bool flash_is_write_protected(uint32_t address) {
    if (!write_protection_enabled) {
        return false;
    }

    return (address >= protected_start) && (address < (protected_start + protected_size));
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

bool flash_is_busy(void) {
    /* Simulated flash is never busy */
    return false;
}

flash_error_t flash_wait_ready(uint32_t timeout_ms) {
    /* Simulated flash is always ready */
    (void)timeout_ms;
    return FLASH_OK;
}

const char *flash_error_to_string(flash_error_t error) {
    if (error >= 0 && error < sizeof(error_strings) / sizeof(error_strings[0])) {
        return error_strings[error];
    }
    return "Unknown error";
}

/* ============================================================================
 * Weak Platform-Specific Functions (can be overridden)
 * ============================================================================ */

__attribute__((weak)) flash_error_t platform_flash_init(void) {
    /* Default: use simulated flash */
    return FLASH_OK;
}

__attribute__((weak)) flash_error_t platform_flash_read(uint32_t address, void *buffer, size_t size) {
    /* Return error to use default implementation */
    (void)address;
    (void)buffer;
    (void)size;
    return FLASH_ERROR_INVALID_PARAM;
}

__attribute__((weak)) flash_error_t platform_flash_write(uint32_t address, const void *buffer, size_t size) {
    /* Return error to use default implementation */
    (void)address;
    (void)buffer;
    (void)size;
    return FLASH_ERROR_INVALID_PARAM;
}

__attribute__((weak)) flash_error_t platform_flash_erase_sector(uint32_t address) {
    /* Return error to use default implementation */
    (void)address;
    return FLASH_ERROR_INVALID_PARAM;
}
