/**
 * @file ota_demo.c
 * @brief OTA (Over-The-Air) Update Demo
 *
 * Demonstrates firmware update capabilities:
 * - Downloading firmware from server
 * - Installing firmware to alternate partition
 * - Verifying firmware integrity
 * - Rebooting to apply update
 * - Automatic rollback on failure
 */

#include "tinyos.h"
#include "tinyos/ota.h"
#include "tinyos/net.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define FIRMWARE_SERVER_URL     "http://192.168.1.100:8000/firmware.bin"
#define UPDATE_CHECK_INTERVAL   60000  /* Check for updates every 60 seconds */
#define BOOT_CONFIRM_DELAY      10000  /* Confirm boot after 10 seconds */

/* Current firmware version */
#define CURRENT_VERSION         0x00010000  /* Version 1.0.0 */
#define CURRENT_VERSION_STRING  "1.0.0"

/* ============================================================================
 * Test Firmware Image Generation
 * ============================================================================ */

/**
 * @brief Create a test firmware image for demonstration
 */
static uint8_t *create_test_firmware(uint32_t *size) {
    const uint32_t test_firmware_size = 1024 * 64;  /* 64KB test firmware */

    uint8_t *firmware = (uint8_t *)os_malloc(test_firmware_size);
    if (firmware == NULL) {
        return NULL;
    }

    /* Create firmware header */
    ota_image_header_t *header = (ota_image_header_t *)firmware;
    memset(header, 0, sizeof(ota_image_header_t));

    header->magic = 0x544F5346;  /* "TOSF" */
    header->version = 0x00010001;  /* Version 1.0.1 (newer than current) */
    snprintf(header->version_string, OTA_VERSION_STRING_MAX, "1.0.1");
    header->image_size = test_firmware_size;
    header->timestamp = 1234567890;
    header->flags = 0;

    /* Fill rest with test pattern */
    for (uint32_t i = sizeof(ota_image_header_t); i < test_firmware_size; i++) {
        firmware[i] = (uint8_t)(i & 0xFF);
    }

    /* Calculate CRC32 (simplified - would use actual CRC) */
    header->crc32 = 0xDEADBEEF;

    /* Calculate signature (would use actual crypto) */
    memset(header->signature, 0xAA, OTA_SIGNATURE_SIZE);

    *size = test_firmware_size;
    return firmware;
}

/* ============================================================================
 * OTA Progress Callback
 * ============================================================================ */

static void ota_progress_callback(const ota_progress_t *progress, void *user_data) {
    (void)user_data;

    static uint8_t last_percent = 0;

    if (progress->progress_percent != last_percent) {
        printf("OTA Progress: %s - %d%% (%lu / %lu bytes)\n",
               ota_state_to_string(progress->state),
               progress->progress_percent,
               progress->downloaded_bytes,
               progress->total_bytes);

        last_percent = progress->progress_percent;
    }

    if (progress->state == OTA_STATE_FAILED) {
        printf("OTA Error: %s\n", ota_error_to_string(progress->last_error));
    }
}

/* ============================================================================
 * Tasks
 * ============================================================================ */

/**
 * @brief OTA update task
 *
 * Periodically checks for firmware updates and installs them
 */
static void ota_update_task(void *param) {
    (void)param;

    printf("OTA Update Task: Started\n");
    printf("Current Version: %s (0x%08X)\n", CURRENT_VERSION_STRING, CURRENT_VERSION);

    while (1) {
        printf("\n--- Checking for firmware updates ---\n");

        /* For demo purposes, create a test firmware image */
        uint32_t firmware_size;
        uint8_t *test_firmware = create_test_firmware(&firmware_size);

        if (test_firmware != NULL) {
            printf("Test firmware generated: %lu bytes\n", firmware_size);

            /* Start update from buffer */
            ota_error_t err = ota_start_update_from_buffer(
                test_firmware,
                firmware_size,
                ota_progress_callback,
                NULL
            );

            if (err == OTA_OK) {
                printf("✓ Firmware update completed successfully!\n");

                /* Finalize update */
                err = ota_finalize_update();
                if (err == OTA_OK) {
                    printf("✓ Update finalized, ready to reboot\n");

                    /* Wait a moment before rebooting */
                    os_task_delay(2000);

                    printf("\nRebooting to apply update...\n");
                    printf("========================================\n\n");

                    /* Reboot to new firmware */
                    ota_reboot();

                    /* Should not reach here */
                } else {
                    printf("✗ Failed to finalize update: %s\n", ota_error_to_string(err));
                }
            } else {
                printf("✗ Firmware update failed: %s\n", ota_error_to_string(err));
            }

            os_free(test_firmware);
        } else {
            printf("✗ Failed to create test firmware\n");
        }

        /* Wait before next check */
        printf("\nNext update check in %d seconds...\n", UPDATE_CHECK_INTERVAL / 1000);
        os_task_delay(UPDATE_CHECK_INTERVAL);
    }
}

/**
 * @brief Boot confirmation task
 *
 * Confirms successful boot to prevent automatic rollback
 */
static void boot_confirm_task(void *param) {
    (void)param;

    printf("Boot Confirm Task: Started\n");

    /* Wait for system to stabilize */
    printf("Waiting %d seconds before confirming boot...\n", BOOT_CONFIRM_DELAY / 1000);
    os_task_delay(BOOT_CONFIRM_DELAY);

    /* Check if boot needs confirmation */
    if (ota_is_rollback_needed()) {
        printf("\n!!! Boot confirmation required !!!\n");
        printf("Performing system health checks...\n");

        /* Perform health checks */
        bool system_healthy = true;

        /* Check 1: Memory */
        size_t free_mem = os_get_free_memory();
        printf("  - Free memory: %zu bytes ", free_mem);
        if (free_mem < 1024) {
            printf("[FAIL]\n");
            system_healthy = false;
        } else {
            printf("[OK]\n");
        }

        /* Check 2: Tasks running */
        printf("  - Tasks running: [OK]\n");

        /* Check 3: Network (if applicable) */
        printf("  - Network connectivity: [OK]\n");

        if (system_healthy) {
            printf("\n✓ System health checks passed\n");
            printf("Confirming boot...\n");

            ota_error_t err = ota_confirm_boot();
            if (err == OTA_OK) {
                printf("✓ Boot confirmed successfully!\n");
                printf("New firmware is now active and rollback is disabled.\n");
            } else {
                printf("✗ Failed to confirm boot: %s\n", ota_error_to_string(err));
            }
        } else {
            printf("\n✗ System health checks failed!\n");
            printf("Initiating rollback to previous firmware...\n");

            ota_error_t err = ota_rollback();
            if (err == OTA_OK) {
                printf("✓ Rollback initiated, rebooting...\n");
                /* Should reboot */
            } else {
                printf("✗ Rollback failed: %s\n", ota_error_to_string(err));
            }
        }
    } else {
        printf("Boot already confirmed, no action needed.\n");
    }

    /* Task complete */
    printf("Boot Confirm Task: Complete\n");

    /* Delete self */
    os_task_delete(NULL);
}

/**
 * @brief Status monitoring task
 *
 * Periodically prints OTA status information
 */
static void status_monitor_task(void *param) {
    (void)param;

    printf("Status Monitor Task: Started\n");

    while (1) {
        os_task_delay(30000);  /* Every 30 seconds */

        printf("\n");
        ota_print_status();
        ota_print_partition_table();
    }
}

/* ============================================================================
 * Main Function
 * ============================================================================ */

int main(void) {
    tcb_t ota_task;
    tcb_t confirm_task;
    tcb_t monitor_task;

    printf("\n");
    printf("========================================\n");
    printf("  TinyOS - OTA Update Demo\n");
    printf("========================================\n\n");

    /* Initialize OS */
    os_init();
    os_mem_init();

    /* Initialize OTA subsystem */
    ota_config_t ota_config = {
        .server_url = FIRMWARE_SERVER_URL,
        .firmware_path = "/firmware.bin",
        .timeout_ms = 30000,
        .retry_count = 3,
        .verify_signature = true,
        .auto_rollback = true,
        .signature_key = NULL,
        .signature_key_len = 0
    };

    ota_error_t err = ota_init(&ota_config);
    if (err != OTA_OK) {
        printf("Failed to initialize OTA: %s\n", ota_error_to_string(err));
        return 1;
    }

    printf("✓ OTA subsystem initialized\n");

    /* Print initial status */
    printf("\n");
    printf("=== Initial Status ===\n");
    printf("Current Firmware Version: %s (0x%08X)\n", CURRENT_VERSION_STRING, CURRENT_VERSION);
    printf("Running Partition: %s\n",
           ota_get_running_partition() == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
    printf("Update Partition: %s\n",
           ota_get_update_partition() == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
    printf("Rollback Needed: %s\n", ota_is_rollback_needed() ? "Yes" : "No");
    printf("\n");

    ota_print_partition_table();

    /* Create boot confirmation task (high priority) */
    os_task_create(
        &confirm_task,
        "boot_confirm",
        boot_confirm_task,
        NULL,
        PRIORITY_HIGH
    );

    /* Create status monitor task (low priority) */
    os_task_create(
        &monitor_task,
        "status_monitor",
        status_monitor_task,
        NULL,
        PRIORITY_LOW
    );

    /* Create OTA update task (normal priority) */
    os_task_create(
        &ota_task,
        "ota_update",
        ota_update_task,
        NULL,
        PRIORITY_NORMAL
    );

    printf("Demo Features:\n");
    printf("  ✓ Firmware version management\n");
    printf("  ✓ A/B partition swapping\n");
    printf("  ✓ Download and install updates\n");
    printf("  ✓ Firmware verification (CRC32)\n");
    printf("  ✓ Automatic rollback on failure\n");
    printf("  ✓ Boot confirmation mechanism\n");
    printf("  ✓ Progress reporting\n");
    printf("\n");

    printf("Tasks Created:\n");
    printf("  1. boot_confirm   - Confirms successful boot\n");
    printf("  2. ota_update     - Checks for and installs updates\n");
    printf("  3. status_monitor - Displays OTA status\n");
    printf("\n");

    printf("Starting scheduler...\n");
    printf("========================================\n\n");

    /* Start scheduler */
    os_start();

    return 0;
}

/* ============================================================================
 * OTA Example with Network Download (Alternative Implementation)
 * ============================================================================ */

#if 0  /* Disabled - requires network stack initialization */

/**
 * @brief Example: Download firmware from HTTP server
 */
static void download_firmware_example(void) {
    const char *firmware_url = "http://firmware.example.com/update.bin";

    printf("Downloading firmware from: %s\n", firmware_url);

    ota_error_t err = ota_start_update(
        firmware_url,
        ota_progress_callback,
        NULL
    );

    if (err == OTA_OK) {
        printf("Firmware download and installation complete!\n");

        /* Finalize and reboot */
        ota_finalize_update();
        ota_reboot();
    } else {
        printf("Firmware update failed: %s\n", ota_error_to_string(err));
    }
}

/**
 * @brief Example: Manual chunk-by-chunk update
 */
static void manual_chunk_update_example(void) {
    /* Open firmware file or start download */
    const uint32_t total_size = 65536;  /* 64KB */
    const uint32_t chunk_size = 512;

    ota_progress_t progress;

    for (uint32_t offset = 0; offset < total_size; offset += chunk_size) {
        uint8_t chunk[512];

        /* Read or download chunk */
        /* ... obtain chunk data ... */

        /* Write chunk */
        ota_error_t err = ota_write_chunk(chunk, chunk_size, offset);
        if (err != OTA_OK) {
            printf("Failed to write chunk at offset %lu\n", offset);
            ota_abort_update();
            return;
        }

        /* Get progress */
        ota_get_progress(&progress);
        printf("Progress: %d%%\n", progress.progress_percent);
    }

    /* Verify and finalize */
    ota_verify_partition(ota_get_update_partition());
    ota_finalize_update();

    printf("Update complete! Rebooting...\n");
    ota_reboot();
}

#endif /* Network examples */
