/**
 * RAM Disk Driver for TinyOS File System
 *
 * Provides a simple RAM-based block device for testing and development
 * In production, replace with Flash, EEPROM, or SD card drivers
 */

#include "../include/tinyos.h"
#include <string.h>

/* RAM disk configuration */
#define RAMDISK_BLOCKS      256     /* Number of blocks (256 * 512 = 128KB) */
#define RAMDISK_BLOCK_SIZE  512     /* Block size in bytes */

/* RAM disk storage */
static uint8_t ramdisk_storage[RAMDISK_BLOCKS][RAMDISK_BLOCK_SIZE];
static bool ramdisk_initialized = false;

/**
 * Read blocks from RAM disk
 */
static int ramdisk_read(uint32_t block, void *buffer, uint32_t count) {
    if (!ramdisk_initialized) {
        return -1;
    }

    if (block + count > RAMDISK_BLOCKS) {
        return -1;  /* Out of bounds */
    }

    for (uint32_t i = 0; i < count; i++) {
        memcpy((uint8_t *)buffer + (i * RAMDISK_BLOCK_SIZE),
               ramdisk_storage[block + i],
               RAMDISK_BLOCK_SIZE);
    }

    return 0;  /* Success */
}

/**
 * Write blocks to RAM disk
 */
static int ramdisk_write(uint32_t block, const void *buffer, uint32_t count) {
    if (!ramdisk_initialized) {
        return -1;
    }

    if (block + count > RAMDISK_BLOCKS) {
        return -1;  /* Out of bounds */
    }

    for (uint32_t i = 0; i < count; i++) {
        memcpy(ramdisk_storage[block + i],
               (const uint8_t *)buffer + (i * RAMDISK_BLOCK_SIZE),
               RAMDISK_BLOCK_SIZE);
    }

    return 0;  /* Success */
}

/**
 * Erase blocks (for flash compatibility - just zeros for RAM)
 */
static int ramdisk_erase(uint32_t block, uint32_t count) {
    if (!ramdisk_initialized) {
        return -1;
    }

    if (block + count > RAMDISK_BLOCKS) {
        return -1;  /* Out of bounds */
    }

    for (uint32_t i = 0; i < count; i++) {
        memset(ramdisk_storage[block + i], 0, RAMDISK_BLOCK_SIZE);
    }

    return 0;  /* Success */
}

/**
 * Sync (no-op for RAM disk)
 */
static int ramdisk_sync(void) {
    return 0;  /* Success */
}

/**
 * Get block count
 */
static uint32_t ramdisk_get_block_count(void) {
    return RAMDISK_BLOCKS;
}

/**
 * Initialize RAM disk
 */
void ramdisk_init(void) {
    memset(ramdisk_storage, 0, sizeof(ramdisk_storage));
    ramdisk_initialized = true;
}

/**
 * Get RAM disk block device interface
 */
fs_block_device_t *ramdisk_get_device(void) {
    static fs_block_device_t device = {
        .read = ramdisk_read,
        .write = ramdisk_write,
        .erase = ramdisk_erase,
        .sync = ramdisk_sync,
        .get_block_count = ramdisk_get_block_count
    };

    if (!ramdisk_initialized) {
        ramdisk_init();
    }

    return &device;
}
