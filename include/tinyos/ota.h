/**
 * @file ota.h
 * @brief Over-The-Air (OTA) Firmware Update API
 *
 * Provides secure firmware update capabilities with:
 * - A/B partition management
 * - Signature verification
 * - Automatic rollback on failure
 * - Progress monitoring
 */

#ifndef TINYOS_OTA_H
#define TINYOS_OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "tinyos.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants and Definitions
 * ============================================================================ */

#define OTA_VERSION_STRING_MAX  32
#define OTA_SIGNATURE_SIZE      32   /* SHA-256 signature size */
#define OTA_CHUNK_SIZE          512  /* Firmware chunk size for download */

/* ============================================================================
 * Enums and Types
 * ============================================================================ */

/**
 * @brief OTA error codes
 */
typedef enum {
    OTA_OK = 0,
    OTA_ERROR_INVALID_PARAM,
    OTA_ERROR_NOT_INITIALIZED,
    OTA_ERROR_FLASH_ERROR,
    OTA_ERROR_DOWNLOAD_FAILED,
    OTA_ERROR_VERIFICATION_FAILED,
    OTA_ERROR_NO_SPACE,
    OTA_ERROR_INVALID_IMAGE,
    OTA_ERROR_ROLLBACK_FAILED,
    OTA_ERROR_TIMEOUT,
    OTA_ERROR_NETWORK,
    OTA_ERROR_BUSY
} ota_error_t;

/**
 * @brief OTA partition types
 */
typedef enum {
    OTA_PARTITION_BOOTLOADER = 0,
    OTA_PARTITION_APP_A,
    OTA_PARTITION_APP_B,
    OTA_PARTITION_DATA,
    OTA_PARTITION_MAX
} ota_partition_type_t;

/**
 * @brief OTA partition state
 */
typedef enum {
    OTA_PARTITION_STATE_INVALID = 0,
    OTA_PARTITION_STATE_VALID,
    OTA_PARTITION_STATE_PENDING,
    OTA_PARTITION_STATE_ACTIVE,
    OTA_PARTITION_STATE_FAILED
} ota_partition_state_t;

/**
 * @brief OTA update state
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_WRITING,
    OTA_STATE_COMPLETE,
    OTA_STATE_FAILED,
    OTA_STATE_ROLLING_BACK
} ota_state_t;

/**
 * @brief Firmware image header
 */
typedef struct {
    uint32_t magic;                             /* Magic number: 0x544F5346 "TOSF" */
    uint32_t version;                           /* Firmware version */
    char version_string[OTA_VERSION_STRING_MAX]; /* Version string (e.g., "1.2.3") */
    uint32_t image_size;                        /* Firmware image size */
    uint32_t crc32;                             /* CRC32 checksum */
    uint8_t signature[OTA_SIGNATURE_SIZE];      /* SHA-256 signature */
    uint32_t timestamp;                         /* Build timestamp */
    uint32_t flags;                             /* Feature flags */
    uint32_t reserved[4];                       /* Reserved for future use */
} ota_image_header_t;

/**
 * @brief OTA partition information
 */
typedef struct {
    ota_partition_type_t type;
    uint32_t start_address;
    uint32_t size;
    ota_partition_state_t state;
    uint32_t version;
    uint32_t boot_count;
    uint32_t last_boot_timestamp;
} ota_partition_info_t;

/**
 * @brief OTA configuration
 */
typedef struct {
    const char *server_url;           /* Update server URL */
    const char *firmware_path;        /* Path to firmware on server */
    uint32_t timeout_ms;              /* Download timeout */
    uint32_t retry_count;             /* Number of retries */
    bool verify_signature;            /* Enable signature verification */
    bool auto_rollback;               /* Enable automatic rollback */
    uint8_t *signature_key;           /* Public key for signature verification */
    uint16_t signature_key_len;       /* Key length */
} ota_config_t;

/**
 * @brief OTA progress information
 */
typedef struct {
    ota_state_t state;
    uint32_t total_bytes;
    uint32_t downloaded_bytes;
    uint32_t written_bytes;
    uint8_t progress_percent;
    ota_error_t last_error;
} ota_progress_t;

/**
 * @brief OTA progress callback
 * @param progress Current progress information
 * @param user_data User-provided data
 */
typedef void (*ota_progress_callback_t)(const ota_progress_t *progress, void *user_data);

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * @brief Initialize OTA subsystem
 * @param config OTA configuration
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_init(const ota_config_t *config);

/**
 * @brief Get current OTA configuration
 * @param config Pointer to store configuration
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_get_config(ota_config_t *config);

/**
 * @brief Update OTA configuration
 * @param config New configuration
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_set_config(const ota_config_t *config);

/* ============================================================================
 * Partition Management
 * ============================================================================ */

/**
 * @brief Get information about a partition
 * @param type Partition type
 * @param info Pointer to store partition info
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_get_partition_info(ota_partition_type_t type, ota_partition_info_t *info);

/**
 * @brief Get currently running partition
 * @return Current partition type
 */
ota_partition_type_t ota_get_running_partition(void);

/**
 * @brief Get next update partition (A/B swap)
 * @return Next partition type for update
 */
ota_partition_type_t ota_get_update_partition(void);

/**
 * @brief Mark partition as valid/bootable
 * @param type Partition type
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_mark_partition_valid(ota_partition_type_t type);

/**
 * @brief Mark partition as invalid
 * @param type Partition type
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_mark_partition_invalid(ota_partition_type_t type);

/* ============================================================================
 * Firmware Update Operations
 * ============================================================================ */

/**
 * @brief Start firmware update from URL
 * @param url Firmware download URL
 * @param callback Progress callback (optional)
 * @param user_data User data for callback
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_start_update(const char *url, ota_progress_callback_t callback, void *user_data);

/**
 * @brief Start firmware update from buffer
 * @param firmware_data Pointer to firmware data
 * @param size Firmware size
 * @param callback Progress callback (optional)
 * @param user_data User data for callback
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_start_update_from_buffer(const uint8_t *firmware_data, uint32_t size,
                                         ota_progress_callback_t callback, void *user_data);

/**
 * @brief Write firmware chunk (for custom download implementations)
 * @param data Chunk data
 * @param size Chunk size
 * @param offset Offset in firmware image
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_write_chunk(const uint8_t *data, uint32_t size, uint32_t offset);

/**
 * @brief Finalize firmware update and prepare for reboot
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_finalize_update(void);

/**
 * @brief Abort ongoing firmware update
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_abort_update(void);

/**
 * @brief Get current update progress
 * @param progress Pointer to store progress info
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_get_progress(ota_progress_t *progress);

/* ============================================================================
 * Verification and Security
 * ============================================================================ */

/**
 * @brief Verify firmware image in partition
 * @param type Partition type to verify
 * @return OTA_OK if valid, error code otherwise
 */
ota_error_t ota_verify_partition(ota_partition_type_t type);

/**
 * @brief Verify firmware image header
 * @param header Pointer to image header
 * @return OTA_OK if valid, error code otherwise
 */
ota_error_t ota_verify_image_header(const ota_image_header_t *header);

/**
 * @brief Compute CRC32 of firmware image
 * @param type Partition type
 * @param crc32 Pointer to store CRC32 value
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_compute_crc32(ota_partition_type_t type, uint32_t *crc32);

/**
 * @brief Verify firmware signature
 * @param type Partition type
 * @param public_key Public key for verification
 * @param key_len Key length
 * @return OTA_OK if signature valid, error code otherwise
 */
ota_error_t ota_verify_signature(ota_partition_type_t type,
                                 const uint8_t *public_key, uint16_t key_len);

/* ============================================================================
 * Bootloader Integration
 * ============================================================================ */

/**
 * @brief Set boot partition for next reboot
 * @param type Partition type to boot
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_set_boot_partition(ota_partition_type_t type);

/**
 * @brief Reboot device and apply update
 * @return Does not return on success
 */
ota_error_t ota_reboot(void);

/**
 * @brief Perform rollback to previous firmware
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_rollback(void);

/**
 * @brief Confirm current boot as successful
 * Prevents automatic rollback on next boot
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_confirm_boot(void);

/**
 * @brief Check if firmware needs rollback
 * @return true if rollback needed, false otherwise
 */
bool ota_is_rollback_needed(void);

/* ============================================================================
 * Version Information
 * ============================================================================ */

/**
 * @brief Get current running firmware version
 * @return Version number
 */
uint32_t ota_get_running_version(void);

/**
 * @brief Get current running firmware version string
 * @param buffer Buffer to store version string
 * @param buffer_size Buffer size
 * @return OTA_OK on success, error code otherwise
 */
ota_error_t ota_get_running_version_string(char *buffer, uint32_t buffer_size);

/**
 * @brief Get firmware version from partition
 * @param type Partition type
 * @return Version number (0 if invalid)
 */
uint32_t ota_get_partition_version(ota_partition_type_t type);

/**
 * @brief Compare firmware versions
 * @param version1 First version
 * @param version2 Second version
 * @return < 0 if version1 < version2, 0 if equal, > 0 if version1 > version2
 */
int ota_compare_versions(uint32_t version1, uint32_t version2);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Get error string for error code
 * @param error Error code
 * @return Error string
 */
const char *ota_error_to_string(ota_error_t error);

/**
 * @brief Get state string for OTA state
 * @param state OTA state
 * @return State string
 */
const char *ota_state_to_string(ota_state_t state);

/**
 * @brief Print partition table information
 */
void ota_print_partition_table(void);

/**
 * @brief Print OTA status information
 */
void ota_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_OTA_H */
