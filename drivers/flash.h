/**
 * @file flash.h
 * @brief Flash Memory Driver Interface
 *
 * Provides flash memory operations for firmware storage
 */

#ifndef TINYOS_FLASH_H
#define TINYOS_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define FLASH_PAGE_SIZE         512     /* Flash page size in bytes */
#define FLASH_SECTOR_SIZE       4096    /* Flash sector size in bytes */
#define FLASH_TOTAL_SIZE        (512 * 1024)  /* 512KB total flash */

/* Partition layout */
#define FLASH_BOOTLOADER_START  0x00000000
#define FLASH_BOOTLOADER_SIZE   (16 * 1024)   /* 16KB */

#define FLASH_APP_A_START       0x00004000
#define FLASH_APP_A_SIZE        (240 * 1024)  /* 240KB */

#define FLASH_APP_B_START       0x00040000
#define FLASH_APP_B_SIZE        (240 * 1024)  /* 240KB */

#define FLASH_DATA_START        0x0007C000
#define FLASH_DATA_SIZE         (16 * 1024)   /* 16KB */

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Flash error codes
 */
typedef enum {
    FLASH_OK = 0,
    FLASH_ERROR_INVALID_PARAM,
    FLASH_ERROR_NOT_ALIGNED,
    FLASH_ERROR_OUT_OF_RANGE,
    FLASH_ERROR_WRITE_PROTECTED,
    FLASH_ERROR_ERASE_FAILED,
    FLASH_ERROR_WRITE_FAILED,
    FLASH_ERROR_VERIFY_FAILED,
    FLASH_ERROR_BUSY,
    FLASH_ERROR_TIMEOUT
} flash_error_t;

/**
 * @brief Flash info structure
 */
typedef struct {
    uint32_t total_size;
    uint32_t page_size;
    uint32_t sector_size;
    uint32_t write_alignment;
    bool write_protection_enabled;
} flash_info_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize flash driver
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_init(void);

/**
 * @brief Get flash information
 * @param info Pointer to store flash info
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_get_info(flash_info_t *info);

/* ============================================================================
 * Read Operations
 * ============================================================================ */

/**
 * @brief Read data from flash
 * @param address Start address
 * @param buffer Buffer to store data
 * @param size Number of bytes to read
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_read(uint32_t address, void *buffer, size_t size);

/**
 * @brief Read byte from flash
 * @param address Address to read
 * @param value Pointer to store byte value
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_read_byte(uint32_t address, uint8_t *value);

/**
 * @brief Read word (32-bit) from flash
 * @param address Address to read (must be 4-byte aligned)
 * @param value Pointer to store word value
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_read_word(uint32_t address, uint32_t *value);

/* ============================================================================
 * Write Operations
 * ============================================================================ */

/**
 * @brief Write data to flash
 * @param address Start address
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_write(uint32_t address, const void *buffer, size_t size);

/**
 * @brief Write byte to flash
 * @param address Address to write
 * @param value Byte value
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_write_byte(uint32_t address, uint8_t value);

/**
 * @brief Write word (32-bit) to flash
 * @param address Address to write (must be 4-byte aligned)
 * @param value Word value
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_write_word(uint32_t address, uint32_t value);

/**
 * @brief Write with verification
 * @param address Start address
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_write_verify(uint32_t address, const void *buffer, size_t size);

/* ============================================================================
 * Erase Operations
 * ============================================================================ */

/**
 * @brief Erase flash sector
 * @param address Address within sector to erase
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_erase_sector(uint32_t address);

/**
 * @brief Erase multiple sectors
 * @param start_address Start address
 * @param size Size to erase (rounded up to sector boundary)
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_erase_range(uint32_t start_address, size_t size);

/**
 * @brief Erase entire flash (use with caution!)
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_erase_chip(void);

/* ============================================================================
 * Protection
 * ============================================================================ */

/**
 * @brief Enable write protection for address range
 * @param start_address Start address
 * @param size Protection size
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_enable_write_protection(uint32_t start_address, size_t size);

/**
 * @brief Disable write protection
 * @return FLASH_OK on success, error code otherwise
 */
flash_error_t flash_disable_write_protection(void);

/**
 * @brief Check if address is write-protected
 * @param address Address to check
 * @return true if protected, false otherwise
 */
bool flash_is_write_protected(uint32_t address);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Check if flash is busy
 * @return true if busy, false otherwise
 */
bool flash_is_busy(void);

/**
 * @brief Wait for flash operation to complete
 * @param timeout_ms Timeout in milliseconds
 * @return FLASH_OK if ready, FLASH_ERROR_TIMEOUT if timeout
 */
flash_error_t flash_wait_ready(uint32_t timeout_ms);

/**
 * @brief Get error string
 * @param error Error code
 * @return Error string
 */
const char *flash_error_to_string(flash_error_t error);

/* ============================================================================
 * Platform-Specific Functions (weak symbols)
 * ============================================================================ */

/**
 * @brief Platform-specific flash initialization
 * @return FLASH_OK on success
 */
flash_error_t __attribute__((weak)) platform_flash_init(void);

/**
 * @brief Platform-specific flash read
 */
flash_error_t __attribute__((weak)) platform_flash_read(uint32_t address, void *buffer, size_t size);

/**
 * @brief Platform-specific flash write
 */
flash_error_t __attribute__((weak)) platform_flash_write(uint32_t address, const void *buffer, size_t size);

/**
 * @brief Platform-specific flash erase
 */
flash_error_t __attribute__((weak)) platform_flash_erase_sector(uint32_t address);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_FLASH_H */
