/**
 * TinyOS File System Example
 *
 * Demonstrates:
 * - Formatting and mounting a file system
 * - Creating, writing, and reading files
 * - Directory operations
 * - File statistics
 */

#include "../include/tinyos.h"
#include "../drivers/ramdisk.h"
#include <stdio.h>
#include <string.h>

/* Task stacks */
static tcb_t file_writer_task;
static tcb_t file_reader_task;
static tcb_t dir_browser_task;

/* Shared data */
static semaphore_t fs_ready_sem;

/**
 * File writer task - Creates and writes to files
 */
void file_writer(void *param) {
    (void)param;

    printf("[Writer] Waiting for file system ready...\n");
    os_semaphore_wait(&fs_ready_sem, 5000);

    printf("[Writer] Creating files...\n");

    /* Create and write a sensor data file */
    fs_file_t fd = fs_open("/sensor_log.txt", FS_O_CREAT | FS_O_WRONLY | FS_O_TRUNC);
    if (fd != FS_INVALID_FD) {
        const char *log_data = "Temperature: 25.5C\nHumidity: 60%\nPressure: 1013 hPa\n";
        int32_t written = fs_write(fd, log_data, strlen(log_data));
        printf("[Writer] Wrote %ld bytes to sensor_log.txt\n", written);
        fs_sync(fd);
        fs_close(fd);
    } else {
        printf("[Writer] Failed to create sensor_log.txt\n");
    }

    /* Create a configuration file */
    fd = fs_open("/config.dat", FS_O_CREAT | FS_O_WRONLY);
    if (fd != FS_INVALID_FD) {
        typedef struct {
            uint32_t device_id;
            uint16_t sample_rate;
            uint8_t  enable_wifi;
            uint8_t  power_mode;
        } config_t;

        config_t config = {
            .device_id = 0x12345678,
            .sample_rate = 1000,
            .enable_wifi = 1,
            .power_mode = 2
        };

        fs_write(fd, &config, sizeof(config));
        printf("[Writer] Configuration saved\n");
        fs_close(fd);
    }

    /* Create some log entries */
    for (int i = 0; i < 3; i++) {
        char filename[32];
        snprintf(filename, sizeof(filename), "/log_%d.txt", i);

        fd = fs_open(filename, FS_O_CREAT | FS_O_WRONLY);
        if (fd != FS_INVALID_FD) {
            char log_entry[64];
            snprintf(log_entry, sizeof(log_entry), "Log entry %d at tick %lu\n",
                     i, os_get_tick_count());
            fs_write(fd, log_entry, strlen(log_entry));
            fs_close(fd);
            printf("[Writer] Created %s\n", filename);
        }

        os_task_delay(100);
    }

    printf("[Writer] All files created\n");

    /* Show file system statistics */
    fs_stats_t stats;
    if (fs_get_stats(&stats) == OS_OK) {
        printf("\n[Writer] File System Statistics:\n");
        printf("  Total blocks: %lu\n", stats.total_blocks);
        printf("  Used blocks:  %lu\n", stats.used_blocks);
        printf("  Free blocks:  %lu\n", stats.free_blocks);
        printf("  Block size:   %lu bytes\n", stats.block_size);
        printf("  Total files:  %lu\n", stats.total_files);
        printf("  Free space:   %lu bytes\n", fs_get_free_space());
    }

    while (1) {
        os_task_delay(1000);
    }
}

/**
 * File reader task - Reads and displays file contents
 */
void file_reader(void *param) {
    (void)param;

    printf("[Reader] Waiting for file system ready...\n");
    os_semaphore_wait(&fs_ready_sem, 5000);

    /* Wait for writer to create files */
    os_task_delay(500);

    printf("\n[Reader] Reading files...\n");

    /* Read sensor log */
    fs_file_t fd = fs_open("/sensor_log.txt", FS_O_RDONLY);
    if (fd != FS_INVALID_FD) {
        char buffer[128];
        int32_t bytes_read = fs_read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("[Reader] sensor_log.txt content:\n%s\n", buffer);
        }

        /* Get file size */
        printf("[Reader] File size: %ld bytes\n", fs_size(fd));
        fs_close(fd);
    }

    /* Read configuration */
    fd = fs_open("/config.dat", FS_O_RDONLY);
    if (fd != FS_INVALID_FD) {
        typedef struct {
            uint32_t device_id;
            uint16_t sample_rate;
            uint8_t  enable_wifi;
            uint8_t  power_mode;
        } config_t;

        config_t config;
        if (fs_read(fd, &config, sizeof(config)) == sizeof(config)) {
            printf("[Reader] Configuration:\n");
            printf("  Device ID:    0x%08lX\n", config.device_id);
            printf("  Sample rate:  %u Hz\n", config.sample_rate);
            printf("  WiFi enabled: %s\n", config.enable_wifi ? "Yes" : "No");
            printf("  Power mode:   %u\n", config.power_mode);
        }
        fs_close(fd);
    }

    /* Test seek operations */
    fd = fs_open("/sensor_log.txt", FS_O_RDONLY);
    if (fd != FS_INVALID_FD) {
        char buffer[32];

        /* Seek to middle of file */
        fs_seek(fd, 10, FS_SEEK_SET);
        printf("[Reader] Position after seek: %ld\n", fs_tell(fd));

        /* Read from new position */
        int32_t bytes = fs_read(fd, buffer, 20);
        buffer[bytes] = '\0';
        printf("[Reader] Read from position 10: '%s'\n", buffer);

        fs_close(fd);
    }

    /* Test file statistics */
    fs_stat_t stat;
    if (fs_stat("/sensor_log.txt", &stat) == OS_OK) {
        printf("\n[Reader] File statistics for sensor_log.txt:\n");
        printf("  Type: %s\n", stat.type == FS_TYPE_REGULAR ? "Regular file" : "Directory");
        printf("  Size: %lu bytes\n", stat.size);
        printf("  Blocks: %lu\n", stat.blocks);
        printf("  Modified: tick %lu\n", stat.mtime);
    }

    /* Test rename */
    printf("\n[Reader] Testing rename...\n");
    if (fs_rename("/log_0.txt", "/log_renamed.txt") == OS_OK) {
        printf("[Reader] Successfully renamed log_0.txt to log_renamed.txt\n");
    }

    /* Test remove */
    printf("[Reader] Testing file removal...\n");
    if (fs_remove("/log_1.txt") == OS_OK) {
        printf("[Reader] Successfully removed log_1.txt\n");
    }

    while (1) {
        os_task_delay(1000);
    }
}

/**
 * Directory browser task - Lists directory contents
 */
void dir_browser(void *param) {
    (void)param;

    printf("[Browser] Waiting for file system ready...\n");
    os_semaphore_wait(&fs_ready_sem, 5000);

    /* Wait for files to be created */
    os_task_delay(1000);

    while (1) {
        printf("\n[Browser] Directory listing of root:\n");
        printf("  %-24s %8s %10s\n", "Name", "Type", "Size");
        printf("  ------------------------------------------------\n");

        fs_dir_t dir = fs_opendir("/");
        if (dir) {
            fs_dirent_t entry;
            while (fs_readdir(dir, &entry) == OS_OK) {
                const char *type_str = entry.type == FS_TYPE_REGULAR ? "file" : "dir";
                printf("  %-24s %8s %10lu\n", entry.name, type_str, entry.size);
            }
            fs_closedir(dir);
        } else {
            printf("  Failed to open directory\n");
        }

        /* List every 5 seconds */
        os_task_delay(5000);
    }
}

/**
 * File system initialization task
 */
void fs_init_task(void *param) {
    (void)param;

    printf("\n=== TinyOS File System Demo ===\n\n");

    /* Initialize file system */
    printf("[Init] Initializing file system...\n");
    fs_init();

    /* Get RAM disk device */
    fs_block_device_t *device = ramdisk_get_device();

    /* Format the device */
    printf("[Init] Formatting storage...\n");
    if (fs_format(device) != OS_OK) {
        printf("[Init] Format failed!\n");
        while (1) {
            os_task_delay(1000);
        }
    }

    /* Mount file system */
    printf("[Init] Mounting file system...\n");
    if (fs_mount(device) != OS_OK) {
        printf("[Init] Mount failed!\n");
        while (1) {
            os_task_delay(1000);
        }
    }

    printf("[Init] File system ready!\n\n");

    /* Signal that file system is ready */
    os_semaphore_post(&fs_ready_sem);
    os_semaphore_post(&fs_ready_sem);
    os_semaphore_post(&fs_ready_sem);

    /* This task is done */
    while (1) {
        os_task_delay(10000);
    }
}

int main(void) {
    tcb_t init_task;

    /* Initialize OS */
    os_init();

    /* Initialize semaphore */
    os_semaphore_init(&fs_ready_sem, 0);

    /* Create tasks */
    os_task_create(
        &init_task,
        "fs_init",
        fs_init_task,
        NULL,
        PRIORITY_HIGH
    );

    os_task_create(
        &file_writer_task,
        "file_writer",
        file_writer,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &file_reader_task,
        "file_reader",
        file_reader,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &dir_browser_task,
        "dir_browser",
        dir_browser,
        NULL,
        PRIORITY_LOW
    );

    /* Start scheduler */
    printf("Starting TinyOS...\n");
    os_start();

    return 0;
}
