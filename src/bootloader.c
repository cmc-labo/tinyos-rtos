/**
 * @file bootloader.c
 * @brief Simple Bootloader for OTA Updates
 *
 * This is a simplified bootloader implementation for demonstration.
 * In production, this would be a separate binary stored in the bootloader partition.
 */

#include "tinyos/ota.h"
#include "drivers/flash.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BOOTLOADER_VERSION      0x00010000  /* Version 1.0.0 */
#define MAX_BOOT_ATTEMPTS       3
#define WATCHDOG_TIMEOUT_MS     30000

/* ============================================================================
 * Boot Information (stored in Data Partition)
 * ============================================================================ */

typedef struct {
    uint32_t magic;                      /* Magic: 0x424F4F54 "BOOT" */
    ota_partition_type_t active_partition;
    ota_partition_type_t pending_partition;
    uint32_t boot_count;
    uint32_t rollback_count;
    bool rollback_enabled;
    bool boot_confirmed;
    uint32_t boot_attempts;
    uint32_t last_boot_timestamp;
    uint32_t crc32;
} bootloader_boot_info_t;

/* ============================================================================
 * Static Functions
 * ============================================================================ */

/**
 * @brief Read boot information from flash
 */
static bool bootloader_read_boot_info(bootloader_boot_info_t *boot_info) {
    flash_error_t err = flash_read(FLASH_DATA_START, boot_info, sizeof(bootloader_boot_info_t));
    if (err != FLASH_OK) {
        return false;
    }

    /* Verify magic number */
    if (boot_info->magic != 0x424F4F54) {
        return false;
    }

    /* TODO: Verify CRC32 */

    return true;
}

/**
 * @brief Write boot information to flash
 */
static bool bootloader_write_boot_info(const bootloader_boot_info_t *boot_info) {
    flash_error_t err;

    /* Erase data partition */
    err = flash_erase_sector(FLASH_DATA_START);
    if (err != FLASH_OK) {
        return false;
    }

    /* Write boot info */
    err = flash_write(FLASH_DATA_START, boot_info, sizeof(bootloader_boot_info_t));
    if (err != FLASH_OK) {
        return false;
    }

    return true;
}

/**
 * @brief Verify firmware image in partition
 */
static bool bootloader_verify_partition(ota_partition_type_t partition) {
    ota_partition_info_t partition_info;
    ota_error_t err = ota_get_partition_info(partition, &partition_info);
    if (err != OTA_OK) {
        return false;
    }

    /* Read image header */
    ota_image_header_t header;
    flash_error_t flash_err = flash_read(partition_info.start_address,
                                         &header, sizeof(ota_image_header_t));
    if (flash_err != FLASH_OK) {
        return false;
    }

    /* Verify magic number */
    if (header.magic != 0x544F5346) {  /* "TOSF" */
        return false;
    }

    /* Verify image size */
    if (header.image_size == 0 || header.image_size > partition_info.size) {
        return false;
    }

    /* TODO: Verify CRC32 and signature */

    return true;
}

/**
 * @brief Jump to application
 */
static void bootloader_jump_to_app(uint32_t app_address) {
    /* This function would perform the actual jump to the application
     * On ARM Cortex-M, this involves:
     * 1. Loading the stack pointer from app_address
     * 2. Loading the reset handler address from app_address + 4
     * 3. Jumping to the reset handler
     */

    printf("Bootloader: Jumping to application at 0x%08lX\n", app_address);

    /* Example for ARM Cortex-M (would need platform-specific implementation):
     *
     * typedef void (*app_entry_t)(void);
     * uint32_t *vector_table = (uint32_t *)app_address;
     * uint32_t stack_pointer = vector_table[0];
     * app_entry_t reset_handler = (app_entry_t)vector_table[1];
     *
     * __set_MSP(stack_pointer);
     * reset_handler();
     */

    /* For simulation, we just return */
}

/**
 * @brief Handle boot failure and rollback
 */
static void bootloader_handle_boot_failure(bootloader_boot_info_t *boot_info) {
    printf("Bootloader: Boot failed, attempting rollback...\n");

    /* Increment rollback counter */
    boot_info->rollback_count++;

    /* Switch to alternate partition */
    if (boot_info->active_partition == OTA_PARTITION_APP_A) {
        boot_info->active_partition = OTA_PARTITION_APP_B;
    } else {
        boot_info->active_partition = OTA_PARTITION_APP_A;
    }

    boot_info->pending_partition = boot_info->active_partition;
    boot_info->boot_confirmed = true;
    boot_info->boot_attempts = 0;

    /* Save boot info */
    bootloader_write_boot_info(boot_info);
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

/**
 * @brief Bootloader main function
 *
 * This would normally be called as the entry point after reset.
 * It selects the appropriate firmware partition and boots it.
 */
void bootloader_main(void) {
    bootloader_boot_info_t boot_info;
    ota_partition_type_t boot_partition;
    uint32_t app_address;

    printf("\n");
    printf("========================================\n");
    printf("  TinyOS Bootloader v%d.%d.%d\n",
           (BOOTLOADER_VERSION >> 16) & 0xFF,
           (BOOTLOADER_VERSION >> 8) & 0xFF,
           BOOTLOADER_VERSION & 0xFF);
    printf("========================================\n\n");

    /* Initialize flash */
    flash_error_t flash_err = flash_init();
    if (flash_err != FLASH_OK) {
        printf("Bootloader: Flash initialization failed!\n");
        return;
    }

    /* Read boot information */
    bool boot_info_valid = bootloader_read_boot_info(&boot_info);

    if (!boot_info_valid) {
        printf("Bootloader: Invalid boot info, using defaults\n");

        /* Initialize default boot info */
        memset(&boot_info, 0, sizeof(boot_info));
        boot_info.magic = 0x424F4F54;
        boot_info.active_partition = OTA_PARTITION_APP_A;
        boot_info.pending_partition = OTA_PARTITION_APP_A;
        boot_info.boot_count = 0;
        boot_info.rollback_count = 0;
        boot_info.rollback_enabled = true;
        boot_info.boot_confirmed = true;
        boot_info.boot_attempts = 0;

        /* Save default boot info */
        bootloader_write_boot_info(&boot_info);
    }

    /* Check if there's a pending update */
    if (boot_info.pending_partition != boot_info.active_partition && !boot_info.boot_confirmed) {
        printf("Bootloader: Pending update detected\n");
        boot_partition = boot_info.pending_partition;

        /* Verify the new firmware */
        if (bootloader_verify_partition(boot_partition)) {
            printf("Bootloader: New firmware verified, switching partition\n");
            boot_info.active_partition = boot_partition;
            boot_info.boot_attempts = 0;
        } else {
            printf("Bootloader: New firmware verification failed, staying on current partition\n");
            boot_partition = boot_info.active_partition;
            boot_info.pending_partition = boot_info.active_partition;
            boot_info.boot_confirmed = true;
        }

        bootloader_write_boot_info(&boot_info);
    } else {
        boot_partition = boot_info.active_partition;
    }

    /* Check boot attempts */
    if (!boot_info.boot_confirmed) {
        boot_info.boot_attempts++;

        if (boot_info.boot_attempts >= MAX_BOOT_ATTEMPTS) {
            printf("Bootloader: Max boot attempts reached\n");
            if (boot_info.rollback_enabled) {
                bootloader_handle_boot_failure(&boot_info);
                boot_partition = boot_info.active_partition;
            } else {
                printf("Bootloader: Rollback disabled, cannot recover\n");
                return;
            }
        }

        bootloader_write_boot_info(&boot_info);
    }

    /* Verify selected partition */
    if (!bootloader_verify_partition(boot_partition)) {
        printf("Bootloader: Firmware verification failed!\n");

        if (boot_info.rollback_enabled) {
            bootloader_handle_boot_failure(&boot_info);
            boot_partition = boot_info.active_partition;

            /* Retry verification */
            if (!bootloader_verify_partition(boot_partition)) {
                printf("Bootloader: Rollback partition also invalid, cannot boot\n");
                return;
            }
        } else {
            printf("Bootloader: Rollback disabled, cannot boot\n");
            return;
        }
    }

    /* Increment boot count */
    boot_info.boot_count++;
    bootloader_write_boot_info(&boot_info);

    /* Determine application address */
    ota_partition_info_t partition_info;
    ota_get_partition_info(boot_partition, &partition_info);
    app_address = partition_info.start_address;

    printf("Bootloader: Booting from partition %s (0x%08lX)\n",
           boot_partition == OTA_PARTITION_APP_A ? "APP_A" : "APP_B",
           app_address);
    printf("Bootloader: Boot count: %lu\n", boot_info.boot_count);

    if (!boot_info.boot_confirmed) {
        printf("Bootloader: WARNING - Boot not yet confirmed (attempts: %lu/%d)\n",
               boot_info.boot_attempts, MAX_BOOT_ATTEMPTS);
        printf("Bootloader: Application must call ota_confirm_boot() to prevent rollback\n");
    }

    printf("\n");

    /* Jump to application */
    bootloader_jump_to_app(app_address);
}

/**
 * @brief Initialize bootloader
 *
 * This can be called from the application to initialize bootloader-related
 * functionality without actually performing a boot sequence.
 */
void bootloader_init(void) {
    flash_init();
}

/**
 * @brief Print bootloader information
 */
void bootloader_print_info(void) {
    bootloader_boot_info_t boot_info;

    printf("\n=== Bootloader Information ===\n");
    printf("Version: %d.%d.%d\n",
           (BOOTLOADER_VERSION >> 16) & 0xFF,
           (BOOTLOADER_VERSION >> 8) & 0xFF,
           BOOTLOADER_VERSION & 0xFF);

    if (bootloader_read_boot_info(&boot_info)) {
        printf("Active Partition: %s\n",
               boot_info.active_partition == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
        printf("Pending Partition: %s\n",
               boot_info.pending_partition == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
        printf("Boot Count: %lu\n", boot_info.boot_count);
        printf("Rollback Count: %lu\n", boot_info.rollback_count);
        printf("Boot Confirmed: %s\n", boot_info.boot_confirmed ? "Yes" : "No");
        printf("Boot Attempts: %lu/%d\n", boot_info.boot_attempts, MAX_BOOT_ATTEMPTS);
        printf("Rollback Enabled: %s\n", boot_info.rollback_enabled ? "Yes" : "No");
    } else {
        printf("Boot info not available\n");
    }

    printf("\n");
}
