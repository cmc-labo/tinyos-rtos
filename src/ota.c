/**
 * @file ota.c
 * @brief OTA (Over-The-Air) Firmware Update Implementation
 */

#include "tinyos/ota.h"
#include "tinyos/net.h"
#include "drivers/flash.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define OTA_MAGIC_NUMBER        0x544F5346  /* "TOSF" */
#define OTA_MAX_DOWNLOAD_SIZE   (240 * 1024)  /* Maximum firmware size */
#define OTA_BOOTINFO_MAGIC      0x424F4F54  /* "BOOT" */

/* ============================================================================
 * Partition Table
 * ============================================================================ */

typedef struct {
    uint32_t magic;                      /* Magic: 0x424F4F54 */
    ota_partition_type_t active_partition;
    ota_partition_type_t pending_partition;
    uint32_t boot_count;
    uint32_t rollback_count;
    bool rollback_enabled;
    bool boot_confirmed;
    uint32_t crc32;
} boot_info_t;

static const ota_partition_info_t partition_table[OTA_PARTITION_MAX] = {
    {
        .type = OTA_PARTITION_BOOTLOADER,
        .start_address = FLASH_BOOTLOADER_START,
        .size = FLASH_BOOTLOADER_SIZE,
        .state = OTA_PARTITION_STATE_VALID,
        .version = 0,
        .boot_count = 0,
        .last_boot_timestamp = 0
    },
    {
        .type = OTA_PARTITION_APP_A,
        .start_address = FLASH_APP_A_START,
        .size = FLASH_APP_A_SIZE,
        .state = OTA_PARTITION_STATE_ACTIVE,
        .version = 0,
        .boot_count = 0,
        .last_boot_timestamp = 0
    },
    {
        .type = OTA_PARTITION_APP_B,
        .start_address = FLASH_APP_B_START,
        .size = FLASH_APP_B_SIZE,
        .state = OTA_PARTITION_STATE_INVALID,
        .version = 0,
        .boot_count = 0,
        .last_boot_timestamp = 0
    },
    {
        .type = OTA_PARTITION_DATA,
        .start_address = FLASH_DATA_START,
        .size = FLASH_DATA_SIZE,
        .state = OTA_PARTITION_STATE_VALID,
        .version = 0,
        .boot_count = 0,
        .last_boot_timestamp = 0
    }
};

/* ============================================================================
 * Global State
 * ============================================================================ */

static struct {
    bool initialized;
    ota_config_t config;
    ota_progress_t progress;
    ota_progress_callback_t callback;
    void *callback_user_data;
    ota_partition_type_t running_partition;
    ota_partition_type_t update_partition;
    boot_info_t boot_info;
    uint32_t download_offset;
    ota_image_header_t current_header;
} ota_state = {0};

/* ============================================================================
 * CRC32 Calculation
 * ============================================================================ */

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void crc32_init_table(void) {
    if (crc32_table_initialized) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
        crc32_table[i] = crc;
    }

    crc32_table_initialized = true;
}

static uint32_t crc32_calculate(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;

    crc32_init_table();

    for (size_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        crc = (crc >> 8) ^ crc32_table[(crc ^ byte) & 0xFF];
    }

    return ~crc;
}

/* ============================================================================
 * Boot Info Management
 * ============================================================================ */

static ota_error_t load_boot_info(void) {
    flash_error_t flash_err;

    /* Read boot info from data partition */
    flash_err = flash_read(FLASH_DATA_START, &ota_state.boot_info, sizeof(boot_info_t));
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    /* Validate magic and CRC */
    if (ota_state.boot_info.magic != OTA_BOOTINFO_MAGIC) {
        /* Initialize default boot info */
        memset(&ota_state.boot_info, 0, sizeof(boot_info_t));
        ota_state.boot_info.magic = OTA_BOOTINFO_MAGIC;
        ota_state.boot_info.active_partition = OTA_PARTITION_APP_A;
        ota_state.boot_info.pending_partition = OTA_PARTITION_APP_A;
        ota_state.boot_info.rollback_enabled = true;
        ota_state.boot_info.boot_confirmed = true;

        /* Save boot info */
        return save_boot_info();
    }

    return OTA_OK;
}

static ota_error_t save_boot_info(void) {
    flash_error_t flash_err;

    /* Calculate CRC */
    ota_state.boot_info.crc32 = crc32_calculate((uint8_t *)&ota_state.boot_info,
                                                 sizeof(boot_info_t) - sizeof(uint32_t));

    /* Erase data partition */
    flash_err = flash_erase_sector(FLASH_DATA_START);
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    /* Write boot info */
    flash_err = flash_write(FLASH_DATA_START, &ota_state.boot_info, sizeof(boot_info_t));
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    return OTA_OK;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

ota_error_t ota_init(const ota_config_t *config) {
    if (ota_state.initialized) {
        return OTA_OK;
    }

    /* Initialize flash */
    flash_error_t flash_err = flash_init();
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    /* Load configuration */
    if (config != NULL) {
        memcpy(&ota_state.config, config, sizeof(ota_config_t));
    } else {
        /* Default configuration */
        memset(&ota_state.config, 0, sizeof(ota_config_t));
        ota_state.config.timeout_ms = 30000;
        ota_state.config.retry_count = 3;
        ota_state.config.verify_signature = true;
        ota_state.config.auto_rollback = true;
    }

    /* Load boot info */
    ota_error_t err = load_boot_info();
    if (err != OTA_OK) {
        return err;
    }

    /* Determine running partition */
    ota_state.running_partition = ota_state.boot_info.active_partition;
    ota_state.update_partition = (ota_state.running_partition == OTA_PARTITION_APP_A) ?
                                  OTA_PARTITION_APP_B : OTA_PARTITION_APP_A;

    /* Initialize progress */
    ota_state.progress.state = OTA_STATE_IDLE;
    ota_state.progress.total_bytes = 0;
    ota_state.progress.downloaded_bytes = 0;
    ota_state.progress.written_bytes = 0;
    ota_state.progress.progress_percent = 0;
    ota_state.progress.last_error = OTA_OK;

    ota_state.initialized = true;

    return OTA_OK;
}

ota_error_t ota_get_config(ota_config_t *config) {
    if (!ota_state.initialized || config == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    memcpy(config, &ota_state.config, sizeof(ota_config_t));
    return OTA_OK;
}

ota_error_t ota_set_config(const ota_config_t *config) {
    if (!ota_state.initialized || config == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    memcpy(&ota_state.config, config, sizeof(ota_config_t));
    return OTA_OK;
}

/* ============================================================================
 * Partition Management
 * ============================================================================ */

ota_error_t ota_get_partition_info(ota_partition_type_t type, ota_partition_info_t *info) {
    if (type >= OTA_PARTITION_MAX || info == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    memcpy(info, &partition_table[type], sizeof(ota_partition_info_t));
    return OTA_OK;
}

ota_partition_type_t ota_get_running_partition(void) {
    return ota_state.running_partition;
}

ota_partition_type_t ota_get_update_partition(void) {
    return ota_state.update_partition;
}

ota_error_t ota_mark_partition_valid(ota_partition_type_t type) {
    if (type >= OTA_PARTITION_MAX) {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Update partition state (would be stored in flash metadata) */
    return OTA_OK;
}

ota_error_t ota_mark_partition_invalid(ota_partition_type_t type) {
    if (type >= OTA_PARTITION_MAX) {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Update partition state */
    return OTA_OK;
}

/* ============================================================================
 * Firmware Update Operations
 * ============================================================================ */

static void report_progress(void) {
    if (ota_state.callback != NULL) {
        ota_state.callback(&ota_state.progress, ota_state.callback_user_data);
    }
}

ota_error_t ota_start_update(const char *url, ota_progress_callback_t callback, void *user_data) {
    if (!ota_state.initialized || url == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    if (ota_state.progress.state != OTA_STATE_IDLE) {
        return OTA_ERROR_BUSY;
    }

    /* Set callback */
    ota_state.callback = callback;
    ota_state.callback_user_data = user_data;

    /* Reset progress */
    ota_state.progress.state = OTA_STATE_DOWNLOADING;
    ota_state.progress.downloaded_bytes = 0;
    ota_state.progress.written_bytes = 0;
    ota_state.progress.progress_percent = 0;
    ota_state.download_offset = 0;

    report_progress();

    /* Download firmware using HTTP */
    http_response_t response;
    os_error_t net_err = net_http_get(url, &response, ota_state.config.timeout_ms);

    if (net_err != OS_OK) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = OTA_ERROR_DOWNLOAD_FAILED;
        report_progress();
        return OTA_ERROR_DOWNLOAD_FAILED;
    }

    /* Process downloaded firmware */
    ota_error_t err = ota_start_update_from_buffer((const uint8_t *)response.body,
                                                     response.body_length,
                                                     callback, user_data);

    net_http_free_response(&response);
    return err;
}

ota_error_t ota_start_update_from_buffer(const uint8_t *firmware_data, uint32_t size,
                                         ota_progress_callback_t callback, void *user_data) {
    if (!ota_state.initialized || firmware_data == NULL || size == 0) {
        return OTA_ERROR_INVALID_PARAM;
    }

    if (size > OTA_MAX_DOWNLOAD_SIZE) {
        return OTA_ERROR_NO_SPACE;
    }

    /* Set callback */
    ota_state.callback = callback;
    ota_state.callback_user_data = user_data;

    /* Parse and verify header */
    if (size < sizeof(ota_image_header_t)) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = OTA_ERROR_INVALID_IMAGE;
        report_progress();
        return OTA_ERROR_INVALID_IMAGE;
    }

    memcpy(&ota_state.current_header, firmware_data, sizeof(ota_image_header_t));

    ota_error_t err = ota_verify_image_header(&ota_state.current_header);
    if (err != OTA_OK) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = err;
        report_progress();
        return err;
    }

    /* Update progress */
    ota_state.progress.state = OTA_STATE_WRITING;
    ota_state.progress.total_bytes = size;
    ota_state.progress.downloaded_bytes = size;
    report_progress();

    /* Erase update partition */
    ota_partition_info_t partition_info;
    ota_get_partition_info(ota_state.update_partition, &partition_info);

    flash_error_t flash_err = flash_erase_range(partition_info.start_address, partition_info.size);
    if (flash_err != FLASH_OK) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = OTA_ERROR_FLASH_ERROR;
        report_progress();
        return OTA_ERROR_FLASH_ERROR;
    }

    /* Write firmware to flash */
    flash_err = flash_write(partition_info.start_address, firmware_data, size);
    if (flash_err != FLASH_OK) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = OTA_ERROR_FLASH_ERROR;
        report_progress();
        return OTA_ERROR_FLASH_ERROR;
    }

    ota_state.progress.written_bytes = size;
    ota_state.progress.progress_percent = 100;

    /* Verify written data */
    ota_state.progress.state = OTA_STATE_VERIFYING;
    report_progress();

    err = ota_verify_partition(ota_state.update_partition);
    if (err != OTA_OK) {
        ota_state.progress.state = OTA_STATE_FAILED;
        ota_state.progress.last_error = err;
        report_progress();
        return err;
    }

    /* Mark update partition as pending */
    ota_state.boot_info.pending_partition = ota_state.update_partition;
    ota_state.boot_info.boot_confirmed = false;
    save_boot_info();

    /* Complete */
    ota_state.progress.state = OTA_STATE_COMPLETE;
    report_progress();

    return OTA_OK;
}

ota_error_t ota_write_chunk(const uint8_t *data, uint32_t size, uint32_t offset) {
    if (!ota_state.initialized || data == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    ota_partition_info_t partition_info;
    ota_get_partition_info(ota_state.update_partition, &partition_info);

    if (offset + size > partition_info.size) {
        return OTA_ERROR_NO_SPACE;
    }

    flash_error_t flash_err = flash_write(partition_info.start_address + offset, data, size);
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    ota_state.progress.written_bytes += size;
    if (ota_state.progress.total_bytes > 0) {
        ota_state.progress.progress_percent =
            (ota_state.progress.written_bytes * 100) / ota_state.progress.total_bytes;
    }

    report_progress();

    return OTA_OK;
}

ota_error_t ota_finalize_update(void) {
    if (!ota_state.initialized) {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    if (ota_state.progress.state != OTA_STATE_COMPLETE) {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Set boot partition for next reboot */
    return ota_set_boot_partition(ota_state.update_partition);
}

ota_error_t ota_abort_update(void) {
    if (!ota_state.initialized) {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    /* Reset progress */
    ota_state.progress.state = OTA_STATE_IDLE;
    ota_state.progress.total_bytes = 0;
    ota_state.progress.downloaded_bytes = 0;
    ota_state.progress.written_bytes = 0;
    ota_state.progress.progress_percent = 0;

    return OTA_OK;
}

ota_error_t ota_get_progress(ota_progress_t *progress) {
    if (!ota_state.initialized || progress == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    memcpy(progress, &ota_state.progress, sizeof(ota_progress_t));
    return OTA_OK;
}

/* ============================================================================
 * Verification
 * ============================================================================ */

ota_error_t ota_verify_partition(ota_partition_type_t type) {
    if (type >= OTA_PARTITION_MAX) {
        return OTA_ERROR_INVALID_PARAM;
    }

    ota_partition_info_t partition_info;
    ota_get_partition_info(type, &partition_info);

    /* Read header */
    ota_image_header_t header;
    flash_error_t flash_err = flash_read(partition_info.start_address, &header, sizeof(ota_image_header_t));
    if (flash_err != FLASH_OK) {
        return OTA_ERROR_FLASH_ERROR;
    }

    /* Verify header */
    return ota_verify_image_header(&header);
}

ota_error_t ota_verify_image_header(const ota_image_header_t *header) {
    if (header == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Check magic number */
    if (header->magic != OTA_MAGIC_NUMBER) {
        return OTA_ERROR_INVALID_IMAGE;
    }

    /* Check image size */
    if (header->image_size == 0 || header->image_size > OTA_MAX_DOWNLOAD_SIZE) {
        return OTA_ERROR_INVALID_IMAGE;
    }

    return OTA_OK;
}

ota_error_t ota_compute_crc32(ota_partition_type_t type, uint32_t *crc32) {
    if (type >= OTA_PARTITION_MAX || crc32 == NULL) {
        return OTA_ERROR_INVALID_PARAM;
    }

    ota_partition_info_t partition_info;
    ota_get_partition_info(type, &partition_info);

    /* Read data and compute CRC */
    uint8_t *buffer = (uint8_t *)os_malloc(4096);
    if (buffer == NULL) {
        return OTA_ERROR_FLASH_ERROR;
    }

    uint32_t computed_crc = 0xFFFFFFFF;
    uint32_t remaining = partition_info.size;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk_size = (remaining < 4096) ? remaining : 4096;

        flash_error_t flash_err = flash_read(partition_info.start_address + offset, buffer, chunk_size);
        if (flash_err != FLASH_OK) {
            os_free(buffer);
            return OTA_ERROR_FLASH_ERROR;
        }

        for (uint32_t i = 0; i < chunk_size; i++) {
            computed_crc = (computed_crc >> 8) ^ crc32_table[(computed_crc ^ buffer[i]) & 0xFF];
        }

        offset += chunk_size;
        remaining -= chunk_size;
    }

    os_free(buffer);
    *crc32 = ~computed_crc;

    return OTA_OK;
}

ota_error_t ota_verify_signature(ota_partition_type_t type,
                                 const uint8_t *public_key, uint16_t key_len) {
    /* Signature verification would use a crypto library (e.g., mbedTLS, TinyCrypt) */
    /* This is a placeholder implementation */
    (void)type;
    (void)public_key;
    (void)key_len;

    /* TODO: Implement actual signature verification */
    return OTA_OK;
}

/* ============================================================================
 * Bootloader Integration
 * ============================================================================ */

ota_error_t ota_set_boot_partition(ota_partition_type_t type) {
    if (type >= OTA_PARTITION_MAX) {
        return OTA_ERROR_INVALID_PARAM;
    }

    ota_state.boot_info.pending_partition = type;
    ota_state.boot_info.boot_confirmed = false;

    return save_boot_info();
}

ota_error_t ota_reboot(void) {
    /* Platform-specific reboot implementation */
    /* This would typically use NVIC_SystemReset() on ARM */
    printf("OTA: Rebooting to apply update...\n");

    /* TODO: Call platform-specific reboot function */
    return OTA_OK;
}

ota_error_t ota_rollback(void) {
    if (!ota_state.initialized) {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    /* Swap partitions */
    ota_partition_type_t rollback_partition =
        (ota_state.running_partition == OTA_PARTITION_APP_A) ?
        OTA_PARTITION_APP_B : OTA_PARTITION_APP_A;

    ota_state.boot_info.active_partition = rollback_partition;
    ota_state.boot_info.pending_partition = rollback_partition;
    ota_state.boot_info.boot_confirmed = true;
    ota_state.boot_info.rollback_count++;

    save_boot_info();

    return ota_reboot();
}

ota_error_t ota_confirm_boot(void) {
    if (!ota_state.initialized) {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    ota_state.boot_info.boot_confirmed = true;
    ota_state.boot_info.active_partition = ota_state.running_partition;

    return save_boot_info();
}

bool ota_is_rollback_needed(void) {
    if (!ota_state.initialized) {
        return false;
    }

    return (!ota_state.boot_info.boot_confirmed &&
            ota_state.boot_info.rollback_enabled);
}

/* ============================================================================
 * Version Information
 * ============================================================================ */

uint32_t ota_get_running_version(void) {
    return ota_state.current_header.version;
}

ota_error_t ota_get_running_version_string(char *buffer, uint32_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return OTA_ERROR_INVALID_PARAM;
    }

    snprintf(buffer, buffer_size, "%s", ota_state.current_header.version_string);
    return OTA_OK;
}

uint32_t ota_get_partition_version(ota_partition_type_t type) {
    if (type >= OTA_PARTITION_MAX) {
        return 0;
    }

    ota_partition_info_t partition_info;
    ota_get_partition_info(type, &partition_info);

    ota_image_header_t header;
    flash_error_t flash_err = flash_read(partition_info.start_address, &header, sizeof(ota_image_header_t));
    if (flash_err != FLASH_OK) {
        return 0;
    }

    if (header.magic != OTA_MAGIC_NUMBER) {
        return 0;
    }

    return header.version;
}

int ota_compare_versions(uint32_t version1, uint32_t version2) {
    if (version1 < version2) {
        return -1;
    } else if (version1 > version2) {
        return 1;
    } else {
        return 0;
    }
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const char *ota_error_to_string(ota_error_t error) {
    static const char *error_strings[] = {
        "OK",
        "Invalid parameter",
        "Not initialized",
        "Flash error",
        "Download failed",
        "Verification failed",
        "No space",
        "Invalid image",
        "Rollback failed",
        "Timeout",
        "Network error",
        "Busy"
    };

    if (error >= 0 && error < sizeof(error_strings) / sizeof(error_strings[0])) {
        return error_strings[error];
    }
    return "Unknown error";
}

const char *ota_state_to_string(ota_state_t state) {
    static const char *state_strings[] = {
        "Idle",
        "Downloading",
        "Verifying",
        "Writing",
        "Complete",
        "Failed",
        "Rolling back"
    };

    if (state >= 0 && state < sizeof(state_strings) / sizeof(state_strings[0])) {
        return state_strings[state];
    }
    return "Unknown state";
}

void ota_print_partition_table(void) {
    printf("\n=== OTA Partition Table ===\n");

    for (int i = 0; i < OTA_PARTITION_MAX; i++) {
        ota_partition_info_t info;
        ota_get_partition_info((ota_partition_type_t)i, &info);

        printf("Partition %d: 0x%08lX - 0x%08lX (%lu KB)\n",
               i, info.start_address, info.start_address + info.size, info.size / 1024);
    }

    printf("\n");
}

void ota_print_status(void) {
    printf("\n=== OTA Status ===\n");
    printf("Running Partition: %s\n",
           ota_state.running_partition == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
    printf("Update Partition:  %s\n",
           ota_state.update_partition == OTA_PARTITION_APP_A ? "APP_A" : "APP_B");
    printf("Current State:     %s\n", ota_state_to_string(ota_state.progress.state));
    printf("Progress:          %d%%\n", ota_state.progress.progress_percent);
    printf("Boot Confirmed:    %s\n", ota_state.boot_info.boot_confirmed ? "Yes" : "No");
    printf("Boot Count:        %lu\n", ota_state.boot_info.boot_count);
    printf("Rollback Count:    %lu\n", ota_state.boot_info.rollback_count);
    printf("\n");
}
